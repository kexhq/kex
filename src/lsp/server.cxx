#include "server.hxx"

#include "../common/completion.hxx"
#include "../common/prelude_interfaces.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/version.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/db.hxx"
#include "../semantic/types.hxx"
#include "../validation/tag_validator.hxx"

#include <lsp/connection.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <istream>
#include <iterator>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace kex::lsp {
namespace {

struct Document {
    struct HoverEntry {
        unsigned int line = 0;
        unsigned int byteColumn = 1;
        unsigned int byteLength = 0;
        std::string detail;
        std::string documentationKey;
        bool completeDetail = false;
    };

    // Where a typed expression ENDS, and what to complete against there.
    // `localReceiverTypes` is keyed by name, so it can only answer for
    // receivers that are plain identifiers — `box.` completes, `makeBox(3).`
    // and a chain continued on the next line do not, because there is no name
    // to look up. These spans answer positionally instead, which is what a
    // call result or a multi-line builder chain needs.
    // Where a completable expression STARTS -> what to complete against.
    // Keyed by start because that is what the AST records (nodes carry no end
    // position) and what a backwards scan from the cursor can recover.
    static auto receiverKey(unsigned int line, unsigned int byteColumn)
        -> uint64_t {
        return (static_cast<uint64_t>(line) << 32) | byteColumn;
    }

    std::string path;
    std::string text;
    int version = 0;
    std::vector<HoverEntry> hoverEntries;
    std::vector<HoverEntry> selectedCallEntries;
    std::unordered_map<std::string, std::string> localReceiverTypes;
    std::unordered_map<uint64_t, std::string> receiverSpans;
    // Receivers that needed the full re-analysis, by start offset. A call
    // result (`Date.of(2026, 7, 30).weekday`) has no recorded span, and an
    // editor asks about the same site repeatedly — once per keystroke during
    // completion. Dropped whenever the text changes, so it cannot go stale.
    mutable std::unordered_map<size_t, std::string> recoveredReceivers;
    std::unordered_map<std::string, std::vector<std::string>> documentation;
};

class Iostream final : public ::lsp::io::Stream {
public:
    Iostream(std::istream& input, std::ostream& output)
        : m_input(input), m_output(output) {}

    void read(char* buffer, std::size_t size) override {
        m_input.read(buffer, static_cast<std::streamsize>(size));
        if (m_input.bad()) throw ::lsp::io::Error("failed to read LSP input");
    }

    void write(const char* buffer, std::size_t size) override {
        m_output.write(buffer, static_cast<std::streamsize>(size));
        m_output.flush();
        if (!m_output) throw ::lsp::io::Error("failed to write LSP output");
    }

private:
    std::istream& m_input;
    std::ostream& m_output;
};

auto uriPath(const ::lsp::DocumentUri& uri) -> std::string {
    return uri.isFileUri() ? uri.fsPath() : uri.toString();
}

auto moduleRootsFor(const std::string& filepath) -> std::vector<std::string> {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sourceDir = fs::weakly_canonical(filepath, ec).parent_path();
    if (ec) sourceDir = fs::path(filepath).parent_path();
    std::vector<std::string> roots;
    for (const auto* relative : {"lib", "src"}) {
        const auto candidate = sourceDir / relative;
        ec.clear();
        if (fs::is_directory(candidate, ec) && !ec)
            roots.push_back(candidate.string());
    }
    if (roots.empty()) roots.push_back(sourceDir.string());
    for (auto& root : standardLibraryModuleRoots())
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(std::move(root));
    return roots;
}

// A `package.kex` manifest is written in a vocabulary the stdlib describes
// but no program imports (`bundle`, `version`, `tey`, ...). Loading those
// declarations alongside it is what turns an editor's wall of "undefined
// function" into hover and completion — the same companion mechanism a
// `.spec.kex` uses for its base file.
auto manifestCompanion(const std::string& filepath) -> std::vector<std::string> {
    if (std::filesystem::path(filepath).filename() != "package.kex") return {};
    if (auto vocabulary = kex::manifestVocabularyFile(); !vocabulary.empty())
        return {vocabulary};
    return {};
}

auto specCompanions(const std::string& filepath) -> std::vector<std::string> {
    constexpr std::string_view suffix = ".spec.kex";
    if (!std::string_view(filepath).ends_with(suffix))
        return manifestCompanion(filepath);
    const auto stem = filepath.substr(0, filepath.size() - suffix.size());
    std::vector<std::string> candidates{stem + ".kex"};
    const auto directory = std::filesystem::path(stem).parent_path();
    if (directory.filename() == "spec")
        candidates.push_back(
            (directory.parent_path() / "examples" /
             (std::filesystem::path(stem).filename().string() + ".kex"))
                .string());
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return {candidate};
    }
    return {};
}

auto utf16Column(const std::string& source, int line, int byteColumn) -> int {
    size_t offset = 0;
    for (int current = 1; current < line && offset < source.size(); ++current) {
        const auto newline = source.find('\n', offset);
        if (newline == std::string::npos) return 0;
        offset = newline + 1;
    }
    const auto byteLimit = std::min(
        source.size(), offset + static_cast<size_t>(std::max(0, byteColumn - 1)));
    int units = 0;
    while (offset < byteLimit) {
        const unsigned char lead = source[offset];
        size_t width = 1;
        if ((lead & 0xE0) == 0xC0) width = 2;
        else if ((lead & 0xF0) == 0xE0) width = 3;
        else if ((lead & 0xF8) == 0xF0) width = 4;
        if (offset + width > byteLimit) width = 1;
        units += width == 4 ? 2 : 1;
        offset += width;
    }
    return units;
}

auto byteOffsetForUtf16Column(const std::string& source, size_t lineStart,
                              size_t lineEnd, unsigned int targetUnits) -> size_t {
    auto offset = lineStart;
    unsigned int units = 0;
    while (offset < lineEnd && units < targetUnits) {
        const unsigned char lead = source[offset];
        size_t width = 1;
        if ((lead & 0xE0) == 0xC0) width = 2;
        else if ((lead & 0xF0) == 0xE0) width = 3;
        else if ((lead & 0xF8) == 0xF0) width = 4;
        if (offset + width > lineEnd) width = 1;
        const unsigned int nextUnits = width == 4 ? 2 : 1;
        if (units + nextUnits > targetUnits) break;
        units += nextUnits;
        offset += width;
    }
    return offset;
}

auto protocolPosition(const SourceLocation& location,
                      const std::string* source = nullptr) -> ::lsp::Position {
    return {
        .line = static_cast<unsigned int>(std::max(0, location.line - 1)),
        .character = static_cast<unsigned int>(
            source ? utf16Column(*source, location.line, location.column)
                   : std::max(0, location.column - 1)),
    };
}

// Forward-declared: the token length lives further down, next to the other
// source helpers.
auto identifierLengthAt(const std::string& source, const SourceLocation& location)
    -> unsigned int;

// Where a diagnostic's squiggle ends when the checker reported only a start.
// One character was all it ever underlined, so an error about `parseValue`
// marked a single letter — usually the last one the eye lands on — and said
// nothing about which name it meant. An identifier is underlined whole.
auto pointEnd(const SourceLocation& location,
              const std::string* source = nullptr) -> ::lsp::Position {
    if (source)
        if (const auto length = identifierLengthAt(*source, location);
            length > 1) {
            SourceLocation end = location;
            end.column += static_cast<int>(length);
            return protocolPosition(end, source);
        }
    auto start = protocolPosition(location, source);
    ++start.character;
    return start;
}

auto completionPrefix(const std::string& source, unsigned int line,
                      unsigned int character) -> std::string {
    size_t start = 0;
    for (unsigned int current = 0; current < line && start < source.size(); ++current) {
        const auto newline = source.find('\n', start);
        if (newline == std::string::npos) return {};
        start = newline + 1;
    }
    const auto lineEnd = source.find('\n', start);
    const auto end = lineEnd == std::string::npos ? source.size() : lineEnd;
    const auto cursor = byteOffsetForUtf16Column(source, start, end, character);
    auto begin = cursor;
    while (begin > start) {
        const unsigned char c = source[begin - 1];
        if (!std::isalnum(c) && c != '_' && c != '?' && c != '!') break;
        --begin;
    }
    if (begin > start && source[begin - 1] == '.') {
        auto receiver = begin - 1;
        if (receiver > start && source[receiver - 1] == ']') {
            int depth = 1;
            --receiver;
            while (receiver > start && depth > 0) {
                --receiver;
                if (source[receiver] == ']') ++depth;
                else if (source[receiver] == '[') --depth;
            }
        } else if (receiver > start &&
                   (source[receiver - 1] == '"' || source[receiver - 1] == '\'')) {
            const char quote = source[receiver - 1];
            --receiver;
            while (receiver > start) {
                --receiver;
                if (source[receiver] == quote &&
                    (receiver == start || source[receiver - 1] != '\\'))
                    break;
            }
        } else {
            while (receiver > start) {
                const unsigned char c = source[receiver - 1];
                if (!std::isalnum(c) && c != '_' && c != '.') break;
                --receiver;
            }
        }
        begin = receiver;
    }
    return source.substr(begin, cursor - begin);
}

auto completionByteColumn(const std::string& source, unsigned int line,
                          unsigned int character) -> uint32_t {
    size_t start = 0;
    for (unsigned int current = 0; current < line && start < source.size(); ++current) {
        const auto newline = source.find('\n', start);
        if (newline == std::string::npos) return 1;
        start = newline + 1;
    }
    const auto newline = source.find('\n', start);
    const auto end = newline == std::string::npos ? source.size() : newline;
    return static_cast<uint32_t>(
        byteOffsetForUtf16Column(source, start, end, character) - start + 1);
}

auto sourceLocalReceiverQualifier(const std::string& source,
                                  std::string_view receiver,
                                  size_t beforeOffset) -> std::string {
    const auto needle = "let " + std::string(receiver);
    auto found = source.rfind(needle, beforeOffset);
    while (found != std::string::npos) {
        const bool boundary = found == 0 || source[found - 1] == ' ' ||
            source[found - 1] == '\t' || source[found - 1] == '\n';
        const auto afterName = found + needle.size();
        const bool nameBoundary = afterName >= source.size() ||
            !std::isalnum(static_cast<unsigned char>(source[afterName]));
        if (boundary && nameBoundary) {
            auto cursor = afterName;
            while (cursor < source.size() &&
                   (source[cursor] == ' ' || source[cursor] == '\t'))
                ++cursor;
            if (cursor < source.size() && source[cursor] == ':') {
                ++cursor;
                while (cursor < source.size() && std::isspace(
                           static_cast<unsigned char>(source[cursor])))
                    ++cursor;
                auto end = cursor;
                while (end < source.size() &&
                       (std::isalnum(static_cast<unsigned char>(source[end])) ||
                        source[end] == '_' || source[end] == '.'))
                    ++end;
                if (end > cursor) return source.substr(cursor, end - cursor);
            }
            const auto equals = source.find('=', cursor);
            const auto lineEnd = source.find('\n', cursor);
            if (equals != std::string::npos &&
                (lineEnd == std::string::npos || equals < lineEnd)) {
                cursor = equals + 1;
                while (cursor < source.size() && std::isspace(
                           static_cast<unsigned char>(source[cursor])))
                    ++cursor;
                if (cursor < source.size()) {
                    if (source[cursor] == '[') return "List";
                    if (source[cursor] == '{') return "Map";
                    if (source[cursor] == '"') return "String";
                    if (source[cursor] == '\'') return "Char";
                    if (std::isdigit(static_cast<unsigned char>(source[cursor])))
                        return "Integer";
                    if (std::isupper(static_cast<unsigned char>(source[cursor]))) {
                        auto end = cursor + 1;
                        while (end < source.size() &&
                               (std::isalnum(static_cast<unsigned char>(source[end])) ||
                                source[end] == '_' || source[end] == '.'))
                            ++end;
                        return source.substr(cursor, end - cursor);
                    }
                }
            }
            return {};
        }
        if (!found) break;
        found = source.rfind(needle, found - 1);
    }
    return {};
}

auto sourceLocalDefinition(const std::string& source, const std::string& path,
                           std::string_view name, size_t beforeOffset)
    -> std::optional<SourceLocation> {
    if (name.empty() || source.empty()) return std::nullopt;
    auto identifier = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    auto found = source.rfind(name, std::min(beforeOffset, source.size() - 1));
    while (found != std::string::npos) {
        const auto end = found + name.size();
        const bool boundaries =
            (found == 0 || !identifier(static_cast<unsigned char>(source[found - 1]))) &&
            (end >= source.size() ||
             !identifier(static_cast<unsigned char>(source[end])));
        if (boundaries) {
            const auto lineStartPosition = source.rfind('\n', found);
            const auto lineStart = lineStartPosition == std::string::npos
                ? 0 : lineStartPosition + 1;
            auto prefixEnd = found;
            while (prefixEnd > lineStart && std::isspace(
                       static_cast<unsigned char>(source[prefixEnd - 1])))
                --prefixEnd;
            auto prefixStart = prefixEnd;
            while (prefixStart > lineStart && identifier(
                       static_cast<unsigned char>(source[prefixStart - 1])))
                --prefixStart;
            const auto declarationKeyword = std::string_view(source).substr(
                prefixStart, prefixEnd - prefixStart);

            auto suffix = end;
            while (suffix < source.size() &&
                   (source[suffix] == ' ' || source[suffix] == '\t'))
                ++suffix;
            const auto lineEndPosition = source.find('\n', found);
            const auto lineEnd = lineEndPosition == std::string::npos
                ? source.size() : lineEndPosition;
            const auto line = std::string_view(source).substr(
                lineStart, lineEnd - lineStart);
            const auto relative = found - lineStart;
            const auto declaration = line.find("let ");
            const auto mutation = line.find("var ");
            const auto equals = line.find('=', relative + name.size());
            const auto arrow = line.find("->", relative + name.size());
            const auto openBar = line.rfind('|', relative);
            const auto closeBar = line.find('|', relative + name.size());
            const auto openParen = line.rfind('(', relative);
            const auto closeParen = line.find(')', relative + name.size());
            const bool simpleBinding = declarationKeyword == "let" ||
                                       declarationKeyword == "var";
            const bool destructuredBinding =
                (declaration != std::string_view::npos && declaration < relative ||
                 mutation != std::string_view::npos && mutation < relative) &&
                equals != std::string_view::npos;
            const bool clauseBinding = arrow != std::string_view::npos;
            const bool blockParameter =
                openBar != std::string_view::npos &&
                closeBar != std::string_view::npos && openBar < relative;
            const bool functionParameter =
                declaration != std::string_view::npos && declaration < relative &&
                openParen != std::string_view::npos && openParen < relative &&
                closeParen != std::string_view::npos;
            if (simpleBinding || destructuredBinding || clauseBinding ||
                blockParameter || functionParameter) {
                int line = 1;
                for (size_t cursor = 0; cursor < found; ++cursor)
                    if (source[cursor] == '\n') ++line;
                return SourceLocation{
                    path, line, static_cast<int>(found - lineStart + 1)};
            }
        }
        if (!found) break;
        found = source.rfind(name, found - 1);
    }
    return std::nullopt;
}

// Byte offset of a 1-based line/column, and the inverse. Both appear inline in
// several places already; these are for callers that need to walk the source
// from a node's position and then report what they found.
auto offsetOfLocation(const std::string& source, const SourceLocation& location)
    -> size_t {
    if (location.line < 1 || location.column < 1) return std::string::npos;
    size_t offset = 0;
    for (int line = 1; line < location.line; ++line) {
        const auto newline = source.find('\n', offset);
        if (newline == std::string::npos) return std::string::npos;
        offset = newline + 1;
    }
    offset += static_cast<size_t>(location.column - 1);
    return offset > source.size() ? std::string::npos : offset;
}

auto locationOfOffset(const std::string& source, size_t offset)
    -> std::pair<unsigned int, unsigned int> {
    unsigned int line = 1;
    size_t lineStart = 0;
    for (size_t i = 0; i < offset && i < source.size(); ++i)
        if (source[i] == '\n') { ++line; lineStart = i + 1; }
    return {line, static_cast<unsigned int>(offset - lineStart) + 1};
}

auto identifierLengthAt(const std::string& source, const SourceLocation& location)
    -> unsigned int {
    if (location.line < 1 || location.column < 1) return 0;
    size_t offset = 0;
    for (int line = 1; line < location.line; ++line) {
        const auto newline = source.find('\n', offset);
        if (newline == std::string::npos) return 0;
        offset = newline + 1;
    }
    offset += static_cast<size_t>(location.column - 1);
    if (offset >= source.size()) return 0;
    auto isIdentifier = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    if (!isIdentifier(static_cast<unsigned char>(source[offset]))) return 0;
    const auto start = offset;
    while (offset < source.size() &&
           isIdentifier(static_cast<unsigned char>(source[offset])))
        ++offset;
    return static_cast<unsigned int>(offset - start);
}

auto callNameLocation(const std::string& source, const ast::Expr& expression,
                      const std::string& name, bool method)
    -> std::optional<SourceLocation> {
    if (expression.location.line < 1 || name.empty()) return std::nullopt;
    size_t lineStart = 0;
    for (int line = 1; line < expression.location.line; ++line) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return std::nullopt;
        lineStart = newline + 1;
    }
    const auto newline = source.find('\n', lineStart);
    const auto lineEnd = newline == std::string::npos ? source.size() : newline;
    // Method-call locations may cover the whole receiver chain rather than
    // the final member token. Search the complete source line and use the
    // member boundary below to locate the actual hovered name.
    auto cursor = lineStart;
    auto identifier = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    while ((cursor = source.find(name, cursor)) != std::string::npos &&
           cursor < lineEnd) {
        const bool leftBoundary = cursor == lineStart ||
            !identifier(static_cast<unsigned char>(source[cursor - 1]));
        const bool rightBoundary = cursor + name.size() >= lineEnd ||
            !identifier(static_cast<unsigned char>(source[cursor + name.size()]));
        const bool methodBoundary = !method ||
            (cursor > lineStart && source[cursor - 1] == '.');
        if (leftBoundary && rightBoundary && methodBoundary)
            return SourceLocation{expression.location.file,
                                  expression.location.line,
                                  static_cast<int>(cursor - lineStart + 1)};
        cursor += name.size();
    }
    return std::nullopt;
}

struct WordAtPosition {
    uint32_t byteColumn = 1;
    uint32_t byteLength = 0;
    std::string text;
};

auto wordAt(const std::string& source, unsigned int line,
            unsigned int character) -> WordAtPosition {
    size_t lineStart = 0;
    for (unsigned int current = 0; current < line && lineStart < source.size(); ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    const auto newline = source.find('\n', lineStart);
    const auto lineEnd = newline == std::string::npos ? source.size() : newline;
    auto cursor = byteOffsetForUtf16Column(source, lineStart, lineEnd, character);
    auto isIdentifier = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    if (cursor == lineEnd ||
        (cursor < lineEnd && !isIdentifier(static_cast<unsigned char>(source[cursor])))) {
        if (cursor == lineStart ||
            !isIdentifier(static_cast<unsigned char>(source[cursor - 1])))
            return {};
        --cursor;
    }
    auto start = cursor;
    auto end = cursor + 1;
    while (start > lineStart &&
           isIdentifier(static_cast<unsigned char>(source[start - 1])))
        --start;
    while (end < lineEnd &&
           isIdentifier(static_cast<unsigned char>(source[end])))
        ++end;
    return {
        .byteColumn = static_cast<uint32_t>(start - lineStart + 1),
        .byteLength = static_cast<uint32_t>(end - start),
        .text = source.substr(start, end - start),
    };
}

auto moduleQualifierBeforeWord(const std::string& source, unsigned int line,
                               uint32_t byteColumn) -> std::string {
    size_t lineStart = 0;
    for (unsigned int current = 0; current < line && lineStart < source.size(); ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    const auto wordStart = lineStart + byteColumn - 1;
    if (wordStart <= lineStart || source[wordStart - 1] != '.') return {};
    auto begin = wordStart - 1;
    while (begin > lineStart) {
        const unsigned char c = source[begin - 1];
        if (!std::isalnum(c) && c != '_' && c != '.') break;
        --begin;
    }
    auto qualifier = source.substr(begin, wordStart - begin - 1);
    if (qualifier.empty() ||
        !std::isupper(static_cast<unsigned char>(qualifier.front())))
        return {};
    return qualifier;
}

auto isTypeAnnotationPosition(const std::string& source, unsigned int line,
                              uint32_t byteColumn) -> bool {
    size_t lineStart = 0;
    for (unsigned int current = 0; current < line && lineStart < source.size(); ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return false;
        lineStart = newline + 1;
    }
    const auto wordStart = lineStart + static_cast<size_t>(byteColumn - 1);
    if (wordStart > source.size()) return false;
    auto prefix = std::string_view(source).substr(lineStart, wordStart - lineStart);

    // Parameter, binding, and record-field annotations all introduce their
    // type with ':'. Function results use '->'. Nested generic/list syntax
    // may occur between that marker and the hovered name, so looking at the
    // whole line prefix is intentional.
    const auto colon = prefix.rfind(':');
    const auto arrow = prefix.rfind("->");
    const auto assignment = prefix.rfind('=');
    const auto marker = std::max(colon == std::string_view::npos ? 0 : colon + 1,
                                 arrow == std::string_view::npos ? 0 : arrow + 2);
    return marker > 0 &&
           (assignment == std::string_view::npos || assignment < marker);
}

auto isNamespaceReceiverPosition(const std::string& source, unsigned int line,
                                 uint32_t byteColumn,
                                 uint32_t byteLength) -> bool {
    size_t lineStart = 0;
    for (unsigned int current = 0; current < line && lineStart < source.size(); ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return false;
        lineStart = newline + 1;
    }
    auto offset = lineStart + static_cast<size_t>(byteColumn - 1 + byteLength);
    while (offset < source.size() &&
           (source[offset] == ' ' || source[offset] == '\t'))
        ++offset;
    return offset < source.size() && source[offset] == '.';
}

auto atFieldType(const std::string& source, unsigned int line,
                 uint32_t byteColumn, std::string_view field,
                 const semantic::SemanticDB& db,
                 const std::string& file) -> std::string {
    size_t lineStart = 0;
    for (unsigned int current = 0; current < line; ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    const auto offset = lineStart + static_cast<size_t>(byteColumn - 1);
    if (offset == 0 || offset > source.size() || source[offset - 1] != '@')
        return {};
    auto make = source.rfind("make ", offset);
    if (make == std::string::npos) return {};
    auto targetStart = make + 5;
    while (targetStart < source.size() && std::isspace(
               static_cast<unsigned char>(source[targetStart])))
        ++targetStart;
    auto targetEnd = targetStart;
    while (targetEnd < source.size() &&
           (std::isalnum(static_cast<unsigned char>(source[targetEnd])) ||
            source[targetEnd] == '_' || source[targetEnd] == '.'))
        ++targetEnd;
    if (targetEnd == targetStart) return {};
    const auto* record = db.findSymbol(
        source.substr(targetStart, targetEnd - targetStart), file);
    if (!record || record->kind != semantic::SymbolKind::Record) return {};
    const auto marker = "\n  " + std::string(field) + " : ";
    const auto fieldStart = record->detail.find(marker);
    if (fieldStart == std::string::npos) return {};
    const auto typeStart = fieldStart + marker.size();
    const auto typeEnd = record->detail.find('\n', typeStart);
    return record->detail.substr(typeStart, typeEnd == std::string::npos
        ? std::string::npos : typeEnd - typeStart);
}

auto documentationBeforeSource(std::string_view source,
                               const SourceLocation& definition) -> std::string {
    if (definition.line <= 1) return {};
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= source.size()) {
        const auto end = source.find('\n', start);
        lines.push_back(source.substr(start, end == std::string_view::npos
                                                ? source.size() - start
                                                : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    auto index = static_cast<size_t>(definition.line - 1);
    if (index > lines.size()) index = lines.size();
    std::vector<std::string> docs;
    while (index > 0) {
        auto line = lines[--index];
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line[first] != '#') break;
        line.remove_prefix(first + 1);
        if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        // A section banner separates groups of declarations for a reader; it
        // documents none of them. Collected as documentation it became the
        // first line of a hover — the type `Integer` answered with a rule and
        // a paragraph about durations, because time.kex's `make Integer do`
        // sits under one. A banner ends the walk rather than joining it.
        const auto isBanner = [](std::string_view text) {
            size_t rule = 0;
            for (size_t i = 0; i < text.size(); ++i) {
                // The box-drawing dash is three bytes in UTF-8; count its lead
                // byte only, so a run of them outweighs the title beside it.
                if (static_cast<unsigned char>(text[i]) == 0xE2) {
                    ++rule;
                    i += 2;
                } else if (text[i] == '-' || text[i] == '=' || text[i] == '_') {
                    ++rule;
                }
            }
            return rule >= 8;
        };
        if (isBanner(line)) break;
        docs.emplace_back(line);
    }
    std::reverse(docs.begin(), docs.end());
    std::string result;
    for (const auto& line : docs) {
        if (!result.empty()) result += '\n';
        result += line;
    }
    return result;
}

auto documentationBefore(const semantic::FileState& state,
                         const SourceLocation& definition) -> std::string {
    return documentationBeforeSource(state.source, definition);
}

auto renderRdoc(std::string documentation) -> std::string {
    auto inlineCode = [](std::string line) {
        size_t cursor = 0;
        while ((cursor = line.find('+', cursor)) != std::string::npos) {
            const auto end = line.find('+', cursor + 1);
            if (end == std::string::npos) break;
            if (end == cursor + 1) { cursor = end + 1; continue; }
            line[cursor] = '`';
            line[end] = '`';
            cursor = end + 1;
        }
        return line;
    };

    std::istringstream input(documentation);
    std::string output;
    std::string line;
    bool example = false;
    auto appendLine = [&](const std::string& value = std::string{}) {
        if (!output.empty()) output += '\n';
        output += value;
    };
    auto closeExample = [&]() {
        if (!example) return;
        appendLine("```");
        example = false;
    };
    while (std::getline(input, line)) {
        if (line.rfind("@example", 0) == 0) {
            closeExample();
            appendLine("**Example:**");
            appendLine();
            appendLine("```kex");
            example = true;
            auto inlineExample = line.substr(std::string("@example").size());
            const auto first = inlineExample.find_first_not_of(" \t");
            if (first != std::string::npos)
                appendLine(inlineExample.substr(first));
            continue;
        }
        if (line.rfind("@param ", 0) == 0 ||
            line.rfind("@return", 0) == 0) {
            closeExample();
            const bool parameter = line.rfind("@param ", 0) == 0;
            auto rest = line.substr(parameter ? 7 : 7);
            std::string name;
            if (parameter) {
                const auto separator = rest.find_first_of(" \t");
                name = rest.substr(0, separator);
                rest = separator == std::string::npos ? std::string{} :
                    rest.substr(separator + 1);
            }
            const auto first = rest.find_first_not_of(" \t");
            if (first != std::string::npos) rest.erase(0, first);
            std::string type;
            if (!rest.empty() && rest.front() == '[') {
                if (const auto end = rest.find(']'); end != std::string::npos) {
                    type = rest.substr(1, end - 1);
                    rest.erase(0, end + 1);
                    const auto description = rest.find_first_not_of(" \t");
                    if (description != std::string::npos) rest.erase(0, description);
                    else rest.clear();
                }
            }
            std::string rendered = parameter
                ? "**Parameter `" + name + "`:**"
                : "**Returns:**";
            if (!type.empty()) rendered += " `" + type + "`";
            if (!rest.empty()) rendered += " " + inlineCode(rest);
            appendLine(rendered);
            continue;
        }
        if (example) {
            if (line.empty()) {
                closeExample();
                appendLine();
            } else {
                if (line.rfind("  ", 0) == 0) line.erase(0, 2);
                appendLine(line);
            }
            continue;
        }
        appendLine(inlineCode(line));
    }
    closeExample();
    return output;
}

auto documentedDeclarationName(std::string_view line) -> std::string {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    line.remove_prefix(first);
    if (line.rfind("foul ", 0) == 0) line.remove_prefix(5);
    if (line.rfind("public ", 0) == 0) line.remove_prefix(7);
    if (line.rfind("private ", 0) == 0) line.remove_prefix(8);
    for (const auto keyword : {"module ", "record ", "type ", "trait ", "make "}) {
        if (line.rfind(keyword, 0) != 0) continue;
        line.remove_prefix(std::char_traits<char>::length(keyword));
        const auto end = line.find_first_of(" <(=,\t");
        return std::string(line.substr(0, end));
    }
    if (line.rfind("let ", 0) == 0) line.remove_prefix(4);
    else if (line.rfind("var ", 0) == 0) line.remove_prefix(4);
    else {
        const auto annotation = line.find(" :");
        if (annotation == std::string_view::npos) return {};
        line = line.substr(0, annotation);
    }
    const auto end = line.find_first_of("( =\t");
    auto name = line.substr(0, end);
    if (name.empty()) return {};
    if (!std::isalnum(static_cast<unsigned char>(name.front())) &&
        name.front() != '_') return {};
    return std::string(name);
}

// A comment that is mostly rule characters: `# ── Durations ─────────`. The
// box-drawing dash is three bytes in UTF-8, so only its lead byte is counted —
// a run of them then outweighs the title sitting beside it.
auto isDocumentationBanner(std::string_view text) -> bool {
    size_t rule = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xE2) {
            ++rule;
            i += 2;
        } else if (text[i] == '-' || text[i] == '=' || text[i] == '_') {
            ++rule;
        }
    }
    return rule >= 8;
}

auto sourceDocumentation(const std::string& source)
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::unordered_map<std::string, std::vector<std::string>> result;
    std::istringstream input(source);
    std::vector<std::string> pending;
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '#') {
            auto comment = line.substr(first + 1);
            if (!comment.empty() && comment.front() == ' ')
                comment.erase(0, 1);
            // A section banner documents nothing — it separates groups of
            // declarations for a reader. Kept, it became the opening line of a
            // hover: the type `Integer` answered with a rule and a paragraph
            // about durations, because time.kex's `make Integer do` sits under
            // one. Anything gathered above it belongs to the previous section.
            if (isDocumentationBanner(comment)) {
                pending.clear();
                continue;
            }
            pending.push_back(std::move(comment));
            continue;
        }
        if (first == std::string::npos) {
            if (!pending.empty()) pending.emplace_back();
            continue;
        }
        const auto name = documentedDeclarationName(line);
        if (!name.empty() && !pending.empty()) {
            while (!pending.empty() && pending.back().empty()) pending.pop_back();
            std::string docs;
            for (const auto& comment : pending) {
                if (!docs.empty()) docs += '\n';
                docs += comment;
            }
            auto& entries = result[name];
            if (!docs.empty() &&
                std::find(entries.begin(), entries.end(), docs) == entries.end())
                entries.push_back(std::move(docs));
        }
        pending.clear();
    }
    return result;
}

auto qualifiedSourceDocumentation(const std::string& source,
                                  const std::string& path)
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::unordered_map<std::string, std::vector<std::string>> result;
    Lexer lexer(source, path);
    Parser parser(lexer.tokenizeAll(), path);
    auto program = parser.parseProgram();
    auto add = [&](const std::string& module, const std::string& name,
                   const SourceLocation& location) {
        auto docs = documentationBeforeSource(source, location);
        if (module.empty() || docs.empty()) return;
        auto& entries = result[module + "." + name];
        if (std::find(entries.begin(), entries.end(), docs) == entries.end())
            entries.push_back(std::move(docs));
    };
    std::function<void(const ast::VisibilityBlock&, const std::string&)>
        collectVisibility;
    collectVisibility = [&](const ast::VisibilityBlock& block,
                            const std::string& module) {
        for (const auto& item : block.items)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                  std::unique_ptr<ast::FunctionDef>>)
                    add(module, node->name, node->location);
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::TypeAnnotation>>)
                    if (node->type) add(module, node->name,
                                        node->type->location);
            }, item);
    };
    auto makeTargetName = [](const ast::MakeDef& make) -> std::string {
        if (!make.target) return {};
        if (const auto* name = std::get_if<ast::TypeName>(&make.target->kind))
            return name->parts.empty() ? std::string{} : name->parts.back();
        if (const auto* generic =
                std::get_if<ast::GenericType>(&make.target->kind))
            return generic->name.parts.empty() ? std::string{}
                                                : generic->name.parts.back();
        if (std::holds_alternative<ast::ListType>(make.target->kind))
            return "List";
        if (std::holds_alternative<ast::MapType>(make.target->kind))
            return "Map";
        return {};
    };
    auto collectMake = [&](const ast::MakeDef& make,
                           const std::string& owner) {
        const auto target = owner.empty() ? makeTargetName(make) : owner;
        if (target.empty()) return;
        for (const auto& item : make.body)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                  std::unique_ptr<ast::FunctionDef>>)
                    add(target, node->name, node->location);
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::TypeAnnotation>>) {
                    if (node->type)
                        add(target, node->name, node->type->location);
                }
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::VisibilityBlock>>)
                    collectVisibility(*node, target);
            }, item);
    };
    std::function<void(const ast::ModuleDef&, const std::string&)> collectModule;
    collectModule = [&](const ast::ModuleDef& module,
                        const std::string& parent) {
        const auto qualified = parent.empty() || module.name.find('.') !=
                                                   std::string::npos
            ? module.name : parent + "." + module.name;
        for (const auto& item : module.body)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                  std::unique_ptr<ast::ModuleDef>>)
                    collectModule(*node, qualified);
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::FunctionDef>>)
                    add(qualified, node->name, node->location);
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::TypeAnnotation>>) {
                    if (node->type) add(qualified, node->name,
                                        node->type->location);
                } else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::VisibilityBlock>>)
                    collectVisibility(*node, qualified);
                else if constexpr (std::is_same_v<T,
                                       std::unique_ptr<ast::MakeDef>>)
                    collectMake(*node, qualified);
            }, item);
    };
    for (const auto& item : program.items)
        if (const auto* module =
                std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
            module && *module)
            collectModule(**module, "");
        else if (const auto* make =
                     std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
                 make && *make)
            collectMake(**make, "");
    return result;
}

auto standardLibraryDocumentation()
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::unordered_map<std::string, std::vector<std::string>> result;
    auto files = standardLibrarySourceFiles();
    for (const auto& file : preludeSourceFiles())
        if (std::find(files.begin(), files.end(), file) == files.end())
            files.push_back(file);
    for (const auto& path : files) {
        std::ifstream input(path);
        if (!input) continue;
        std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        for (auto& [name, entries] : sourceDocumentation(source)) {
            auto& destination = result[name];
            for (auto& docs : entries)
                if (std::find(destination.begin(), destination.end(), docs) ==
                    destination.end())
                    destination.push_back(std::move(docs));
        }
        for (auto& [name, entries] : qualifiedSourceDocumentation(source, path)) {
            auto& destination = result[name];
            for (auto& docs : entries)
                if (std::find(destination.begin(), destination.end(), docs) ==
                    destination.end())
                    destination.push_back(std::move(docs));
        }
    }
    return result;
}

auto sourceParameterNames(const std::string& source)
    -> std::unordered_map<std::string, std::vector<std::vector<std::string>>> {
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> result;
    std::istringstream input(source);
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        auto declaration = std::string_view(line).substr(first);
        // `foul` REPLACES `let` on a definition, it does not precede it. When
        // module-level foulness went away every effectful stdlib function was
        // respelled `foul name(...)`, and requiring a `let` after the prefix
        // dropped all of their parameter names from completion.
        if (declaration.rfind("foul ", 0) == 0) declaration.remove_prefix(5);
        else if (declaration.rfind("let ", 0) == 0) declaration.remove_prefix(4);
        else continue;
        const auto open = declaration.find('(');
        const auto close = open == std::string_view::npos
            ? std::string_view::npos : declaration.find(')', open + 1);
        if (open == std::string_view::npos || close == std::string_view::npos)
            continue;
        auto name = std::string(declaration.substr(0, open));
        while (!name.empty() && std::isspace(
                   static_cast<unsigned char>(name.back())))
            name.pop_back();
        if (name.empty()) continue;
        std::vector<std::string> params;
        auto arguments = declaration.substr(open + 1, close - open - 1);
        size_t start = 0;
        while (start < arguments.size()) {
            auto end = arguments.find(',', start);
            if (end == std::string_view::npos) end = arguments.size();
            auto parameter = arguments.substr(start, end - start);
            const auto paramFirst = parameter.find_first_not_of(" \t");
            if (paramFirst != std::string_view::npos) {
                parameter.remove_prefix(paramFirst);
                const auto paramEnd = parameter.find_first_of(":= \t");
                auto paramName = std::string(parameter.substr(0, paramEnd));
                if (!paramName.empty() && paramName != "_")
                    params.push_back(std::move(paramName));
            }
            start = end + 1;
        }
        auto& overloads = result[name];
        if (std::find(overloads.begin(), overloads.end(), params) == overloads.end())
            overloads.push_back(std::move(params));
    }
    return result;
}

auto standardLibraryParameterNames()
    -> std::unordered_map<std::string, std::vector<std::vector<std::string>>> {
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> result;
    auto files = standardLibrarySourceFiles();
    for (const auto& file : preludeSourceFiles())
        if (std::find(files.begin(), files.end(), file) == files.end())
            files.push_back(file);
    for (const auto& path : files) {
        std::ifstream input(path);
        if (!input) continue;
        std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        for (auto& [name, overloads] : sourceParameterNames(source)) {
            auto& destination = result[name];
            for (auto& params : overloads)
                if (std::find(destination.begin(), destination.end(), params) ==
                    destination.end())
                    destination.push_back(std::move(params));
        }
    }
    return result;
}

template <typename Node, typename Callback>
void walkFunctions(const Node& node, Callback& callback);

template <typename Variant, typename Callback>
void walkFunctionVariant(const Variant& item, Callback& callback) {
    std::visit([&](const auto& pointer) {
        if (pointer) walkFunctions(*pointer, callback);
    }, item);
}

template <typename Node, typename Callback>
void walkFunctions(const Node& node, Callback& callback) {
    using T = std::decay_t<Node>;
    if constexpr (std::is_same_v<T, ast::FunctionDef>) {
        callback(node);
    } else if constexpr (std::is_same_v<T, ast::Program> ||
                         std::is_same_v<T, ast::ModuleDef> ||
                         std::is_same_v<T, ast::VisibilityBlock> ||
                         std::is_same_v<T, ast::MakeDef> ||
                         std::is_same_v<T, ast::TraitDef> ||
                         std::is_same_v<T, ast::CompiledBlock>) {
        const auto& items = [&]() -> const auto& {
            if constexpr (std::is_same_v<T, ast::Program>) return node.items;
            else if constexpr (std::is_same_v<T, ast::VisibilityBlock>) return node.items;
            else if constexpr (std::is_same_v<T, ast::CompiledBlock>) return node.items;
            else return node.body;
        }();
        for (const auto& item : items) walkFunctionVariant(item, callback);
    }
}

auto functionDetail(const ast::FunctionDef& function,
                    const std::vector<semantic::Signature>& signatures,
                    const semantic::Analyzer& analyzer)
    -> std::string {
    std::string result;
    for (const auto& signature : signatures) {
        if (!result.empty()) result += '\n';
        if (signature.isFoul || function.isFoul) result += "foul ";
        result += analyzer.displaySignature(function.name, signature);
    }
    return result;
}

auto importedAdtScore(const semantic::ImportedADT& adt) -> size_t {
    size_t score = adt.typeParamNames.size() * 8 +
                   adt.constructorParamTypes.size() * 4 +
                   adt.constructorTypeParamSlots.size() * 2;
    for (const auto& [_, params] : adt.constructorParamTypes)
        score += params.size();
    return score;
}

auto importedAdtNamed(const semantic::ImportedInterfaces& interfaces,
                      const std::string& name)
    -> const semantic::ImportedADT* {
    const semantic::ImportedADT* result = nullptr;
    for (const auto& adt : interfaces.adts)
        if (adt.name == name &&
            (!result || importedAdtScore(adt) > importedAdtScore(*result)))
            result = &adt;
    return result;
}

auto importedAdtWithConstructor(const semantic::ImportedInterfaces& interfaces,
                                const std::string& constructor)
    -> const semantic::ImportedADT* {
    const semantic::ImportedADT* result = nullptr;
    for (const auto& adt : interfaces.adts)
        if (std::find(adt.constructors.begin(), adt.constructors.end(),
                      constructor) != adt.constructors.end() &&
            (!result || importedAdtScore(adt) > importedAdtScore(*result)))
            result = &adt;
    return result;
}

auto importedAdtTypeParameters(const semantic::ImportedADT& adt)
    -> std::vector<std::string> {
    if (!adt.typeParamNames.empty()) return adt.typeParamNames;
    std::vector<std::string> result;
    result.reserve(adt.typeParamCount);
    for (size_t i = 0; i < adt.typeParamCount; ++i)
        result.push_back(std::string(1, static_cast<char>('A' + i % 26)));
    return result;
}

auto importedAdtType(const semantic::ImportedADT& adt,
                     const std::vector<std::string>& parameters) -> std::string {
    std::string result = adt.name;
    if (!parameters.empty()) {
        result += '<';
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i) result += ", ";
            result += parameters[i];
        }
        result += '>';
    }
    return result;
}

auto importedConstructorParameters(const semantic::ImportedADT& adt,
                                   const std::string& constructor,
                                   const std::vector<std::string>& typeParameters)
    -> std::vector<std::string> {
    std::vector<std::string> result;
    const auto slots = adt.constructorTypeParamSlots.find(constructor);
    const auto exact = adt.constructorParamTypes.find(constructor);
    size_t count = 0;
    if (auto arity = adt.constructorArities.find(constructor);
        arity != adt.constructorArities.end())
        count = static_cast<size_t>(std::max(0, arity->second));
    if (slots != adt.constructorTypeParamSlots.end())
        count = std::max(count, slots->second.size());
    if (exact != adt.constructorParamTypes.end())
        count = std::max(count, exact->second.size());
    for (size_t i = 0; i < count; ++i) {
        const int slot = slots != adt.constructorTypeParamSlots.end() &&
                                 i < slots->second.size()
            ? slots->second[i] : -1;
        if (slot >= 0 && static_cast<size_t>(slot) < typeParameters.size())
            result.push_back(typeParameters[slot]);
        else if (exact != adt.constructorParamTypes.end() &&
                 i < exact->second.size())
            result.push_back(semantic::typeToString(exact->second[i]));
        else
            result.push_back("Any");
    }
    return result;
}

auto importedAdtDetail(const semantic::ImportedADT& adt) -> std::string {
    const auto typeParameters = importedAdtTypeParameters(adt);
    std::string result = "type " + importedAdtType(adt, typeParameters) + " =";
    for (size_t i = 0; i < adt.constructors.size(); ++i) {
        const auto& constructor = adt.constructors[i];
        result += i == 0 ? "\n  " : "\n| ";
        result += constructor;
        const auto params = importedConstructorParameters(
            adt, constructor, typeParameters);
        if (!params.empty()) {
            result += '(';
            for (size_t p = 0; p < params.size(); ++p) {
                if (p) result += ", ";
                result += params[p];
            }
            result += ')';
        }
    }
    return result;
}

auto importedConstructorDetail(const semantic::ImportedADT& adt,
                               const std::string& constructor) -> std::string {
    const auto typeParameters = importedAdtTypeParameters(adt);
    const auto params = importedConstructorParameters(
        adt, constructor, typeParameters);
    std::string result = constructor + " : ";
    for (const auto& param : params) result += param + " -> ";
    return result + importedAdtType(adt, typeParameters);
}

auto completionQualifierForType(const semantic::TypePtr& type) -> std::string {
    if (!type) return {};
    return std::visit([&type](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, semantic::ListType>) return "List";
        else if constexpr (std::is_same_v<T, semantic::MapType>) return "Map";
        else if constexpr (std::is_same_v<T, semantic::OptionalType>)
            return "Optional";
        else if constexpr (std::is_same_v<T, semantic::NamedType>)
            return value.name;
        else if constexpr (std::is_same_v<T, semantic::PrimitiveType> ||
                           std::is_same_v<T, semantic::SizedIntType> ||
                           std::is_same_v<T, semantic::SizedFloatType>)
            return semantic::typeToString(type);
        return {};
    }, type->kind);
}

// The declared type of one field, read out of a record symbol's Kex-shaped
// declaration text (`record Box do\n  size : Integer\n  ...`). Hovering a
// field used to answer with whatever global function shared its name — `b.size`
// reported `foul size : String -> Integer?` — because a field is not a symbol
// in its own right and the name-based lookup was all there was.
auto recordFieldType(const std::string& recordDetail, const std::string& field)
    -> std::string {
    size_t lineStart = 0;
    while (lineStart <= recordDetail.size()) {
        const auto newline = recordDetail.find('\n', lineStart);
        const auto end =
            newline == std::string::npos ? recordDetail.size() : newline;
        size_t i = lineStart;
        while (i < end && std::isspace(static_cast<unsigned char>(recordDetail[i])))
            ++i;
        if (recordDetail.compare(i, field.size(), field) == 0) {
            size_t after = i + field.size();
            while (after < end &&
                   std::isspace(static_cast<unsigned char>(recordDetail[after])))
                ++after;
            if (after < end && recordDetail[after] == ':') {
                ++after;
                while (after < end &&
                       std::isspace(static_cast<unsigned char>(recordDetail[after])))
                    ++after;
                // Stop before a default value: `count : Integer = 0`.
                auto stop = recordDetail.find('=', after);
                if (stop == std::string::npos || stop > end) stop = end;
                auto text = recordDetail.substr(after, stop - after);
                while (!text.empty() &&
                       std::isspace(static_cast<unsigned char>(text.back())))
                    text.pop_back();
                if (!text.empty()) return text;
            }
        }
        if (newline == std::string::npos) break;
        lineStart = newline + 1;
    }
    return {};
}

// Whether the word at this position is a KEY in a map literal (`{ name: "Ada" }`)
// rather than a reference to anything. A key is an atom, and it declares
// nothing — but a name-based lookup happily found a same-named export and
// answered with its signature and prose: `name` reported OptionParser's
// documentation about `kex install`.
auto isMapKeyPosition(const std::string& source, unsigned int line,
                      unsigned int byteColumn, size_t wordLength) -> bool {
    size_t lineStart = 0;
    for (unsigned int current = 1; current < line; ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return false;
        lineStart = newline + 1;
    }
    if (byteColumn == 0) return false;
    const size_t start = lineStart + byteColumn - 1;
    size_t after = start + wordLength;
    while (after < source.size() && source[after] == ' ') ++after;
    // `name:` — but not `name ::` or a `?:` conditional.
    if (after >= source.size() || source[after] != ':') return false;
    // Not `::`, and not the `:>` of a method declaration.
    if (after + 1 < source.size() &&
        (source[after + 1] == ':' || source[after + 1] == '>'))
        return false;
    size_t before = start;
    while (before > lineStart && source[before - 1] == ' ') --before;
    if (before == lineStart) return false;
    const char opener = source[before - 1];
    return opener == '{' || opener == ',';
}

// The record a `{ … }` literal names, for a key at this position — `User` in
// `User { name: "Alice" }`. Empty for a plain map literal, whose keys really
// are atoms. Only the same line is examined; a literal opened on an earlier
// line simply falls back to the map reading.
auto literalRecordName(const std::string& source, unsigned int line,
                       unsigned int byteColumn) -> std::string {
    size_t lineStart = 0;
    for (unsigned int current = 1; current < line; ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    if (byteColumn == 0) return {};
    size_t i = lineStart + byteColumn - 1;
    // Back to the `{` that opened this literal, past any earlier `key: value,`.
    while (i > lineStart && source[i - 1] != '{') --i;
    if (i == lineStart || source[i - 1] != '{') return {};
    size_t brace = i - 1;
    while (brace > lineStart && source[brace - 1] == ' ') --brace;
    const size_t nameEnd = brace;
    while (brace > lineStart &&
           (std::isalnum(static_cast<unsigned char>(source[brace - 1])) ||
            source[brace - 1] == '_' || source[brace - 1] == '.'))
        --brace;
    if (brace == nameEnd) return {};
    auto name = source.substr(brace, nameEnd - brace);
    // A record name is capitalised; anything else is not a record literal.
    if (name.empty() || !std::isupper(static_cast<unsigned char>(name.front())))
        return {};
    return name;
}

// `size : Integer` in a record body declares a field. Like a map key it is not
// a reference, so the same name-based lookup answered with an unrelated
// export — hovering `size` reported `foul size : String -> Integer?`. Returns
// the declared type as written, or empty when this is not such a line.
auto declaredFieldType(const std::string& source, unsigned int line,
                       unsigned int byteColumn, size_t wordLength)
    -> std::string {
    size_t lineStart = 0;
    for (unsigned int current = 1; current < line; ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    if (byteColumn == 0) return {};
    const size_t start = lineStart + byteColumn - 1;
    // Nothing but indentation may precede it: an expression that happens to
    // contain `x : y` is not a declaration.
    for (size_t i = lineStart; i < start; ++i)
        if (source[i] != ' ' && source[i] != '\t') return {};
    size_t after = start + wordLength;
    while (after < source.size() && source[after] == ' ') ++after;
    if (after >= source.size() || source[after] != ':') return {};
    // `push :> X -> [X]` DECLARES a method, not a field: taking the text after
    // the colon there produced the nonsense `push : > X -> [X]`.
    if (after + 1 < source.size() &&
        (source[after + 1] == ':' || source[after + 1] == '>'))
        return {};
    ++after;
    while (after < source.size() && source[after] == ' ') ++after;
    const auto lineEnd = source.find('\n', after);
    auto text = source.substr(after, (lineEnd == std::string::npos
                                          ? source.size() : lineEnd) - after);
    // A default value is not part of the type: `count : Integer = 0`.
    if (const auto equals = text.find('='); equals != std::string::npos)
        text = text.substr(0, equals);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

// Module names this file opted into with `using`. Read from the text rather
// than the AST so it still answers while the buffer is mid-edit and cannot be
// parsed — which is exactly when an editor asks about a symbol.
auto documentImports(const std::string& source)
    -> std::unordered_set<std::string> {
    std::unordered_set<std::string> modules;
    size_t lineStart = 0;
    while (lineStart <= source.size()) {
        const auto newline = source.find('\n', lineStart);
        const auto end = newline == std::string::npos ? source.size() : newline;
        size_t i = lineStart;
        while (i < end && std::isspace(static_cast<unsigned char>(source[i]))) ++i;
        if (source.compare(i, 6, "using ") == 0) {
            i += 6;
            while (i < end && std::isspace(static_cast<unsigned char>(source[i]))) ++i;
            const size_t nameStart = i;
            while (i < end &&
                   (std::isalnum(static_cast<unsigned char>(source[i])) ||
                    source[i] == '_' || source[i] == '.'))
                ++i;
            if (i > nameStart) modules.insert(source.substr(nameStart, i - nameStart));
        }
        if (newline == std::string::npos) break;
        lineStart = newline + 1;
    }
    return modules;
}

// Where the receiver of a member completion starts, and the dot it hangs off.
// `completionPrefix` answers this too, but only for receivers made of
// identifiers on the cursor's own line: it stops at `)`, so `makeBox(3).g`
// yields the bare prefix ".g", and it is bounded by the line start, so a
// builder chain continued on the next line yields ".g" as well. Both then
// resolve to nothing. This walks the real shape instead — identifiers,
// balanced `(...)`/`[...]`, and quoted strings, joined by dots, across line
// breaks.
struct DotReceiver {
    size_t start = 0;  // first byte of the receiver expression
    size_t dot = 0;    // the '.' the member being typed hangs off
    // Where the AST records the OUTERMOST expression of the receiver. A call
    // is recorded at its argument list's `(`, not at the receiver it hangs
    // off: in a builder chain `Web.Server.new(0)\n  .get("/", ~h)` the last
    // `.get(…)` call sits at that `(`. Zero when the receiver ends in no call.
    size_t callOpen = 0;
    bool valid = false;
};

auto scanDotReceiver(const std::string& source, size_t cursor) -> DotReceiver {
    const auto isWord = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    const auto skipSpaceBack = [&](size_t i) {
        while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
            --i;
        return i;
    };
    // Offset of the delimiter opening the group that ends just before `end`.
    const auto matchOpen = [&](size_t end, char close, char open) -> size_t {
        int depth = 0;
        for (size_t j = end; j > 0; --j) {
            const char c = source[j - 1];
            if (c == close) {
                ++depth;
            } else if (c == open) {
                --depth;
                if (depth == 0) return j - 1;
            }
        }
        return std::string::npos;
    };

    size_t i = cursor;
    while (i > 0 && isWord(static_cast<unsigned char>(source[i - 1]))) --i;
    i = skipSpaceBack(i);
    if (i == 0 || source[i - 1] != '.') return {};
    const size_t dot = i - 1;

    size_t callOpen = 0;
    i = skipSpaceBack(dot);
    while (i > 0) {
        const char c = source[i - 1];
        // A group is never the whole receiver: `makeBox(3)` continues into the
        // name in front of it, and `[c][0]` into the list before the index.
        if (c == ')' || c == ']') {
            const size_t open = matchOpen(i, c, c == ')' ? '(' : '[');
            if (open == std::string::npos) return {};
            if (c == ')' && callOpen == 0) callOpen = open;
            i = open;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            size_t j = i - 1;
            while (j > 0) {
                --j;
                if (source[j] == c && (j == 0 || source[j - 1] != '\\')) break;
            }
            i = j;
            continue;
        }
        if (!isWord(static_cast<unsigned char>(c))) break;
        while (i > 0 && isWord(static_cast<unsigned char>(source[i - 1]))) --i;
        // `@size` is one expression and the AST records it starting at the
        // `@`. Stopping at the sigil pointed one byte too far right, so the
        // recorded type could not be found and every `@field.method` fell back
        // to re-analyzing the whole buffer.
        if (i > 0 && source[i - 1] == '@') --i;
        // A dot here means the chain continues leftwards: `Web.Server.new(0)`.
        const size_t before = skipSpaceBack(i);
        if (before == 0 || source[before - 1] != '.') break;
        i = skipSpaceBack(before - 1);
    }
    if (i >= dot) return {};
    return {.start = i, .dot = dot, .callOpen = callOpen, .valid = true};
}

// The type of an arbitrary receiver expression, by re-parsing the buffer with
// the half-typed member removed and asking the analyzer. Deleting through the
// cursor rather than just the dot is what makes `makeBox(3).g` parse again.
auto recoveredReceiverQualifier(
    const std::string& source, const std::string& path,
    const DotReceiver& receiver, size_t cursor,
    const semantic::ImportedInterfaces* interfaces,
    const std::vector<std::string>& moduleRoots) -> std::string {
    if (!receiver.valid || cursor < receiver.dot) return {};
    auto recovered = source;
    recovered.erase(receiver.dot, cursor - receiver.dot);
    // Through a SemanticDB with the file's own module roots, not a bare
    // parse: `using Web` is resolved by loading that module's source, and
    // without it every type from an opt-in module came back `unknown` — so a
    // builder chain on `Web.Server` completed to nothing while the same shape
    // on a local record worked. Kept between calls so the module sources are
    // parsed once rather than per keystroke.
    static semantic::SemanticDB recoveryDb;
    recoveryDb.setImportedInterfaces(interfaces);
    recoveryDb.setModuleRoots(moduleRoots);
    recoveryDb.updateFile(path, recovered);
    auto* state = recoveryDb.fileState(path);
    if (!state) return {};
    semantic::Analyzer analyzer(interfaces);
    analyzer.analyze(state->ast);
    const auto& program = state->ast;
    (void)program;

    // Everything before the deletion keeps its position, so the receiver's
    // line/column are the ones it had in the original buffer. A CALL is
    // recorded at its argument list's `(`, so a builder chain
    // (`Web.Server.new(0).get(…).post(…)`) has nothing at all at the start of
    // the receiver — that position is where `Web` is, and the chain's own type
    // lives at the last call's paren.
    const auto positionOf = [&](size_t offset) {
        int line = 1;
        size_t lineStart = 0;
        for (size_t i = 0; i < offset; ++i)
            if (source[i] == '\n') { ++line; lineStart = i + 1; }
        return std::pair<int, int>{
            line, static_cast<int>(offset - lineStart) + 1};
    };
    std::vector<std::pair<int, int>> candidates;
    if (receiver.callOpen) candidates.push_back(positionOf(receiver.callOpen));
    candidates.push_back(positionOf(receiver.start));

    // Several expressions can start at one column — `makeBox` the identifier
    // and `makeBox(3)` the call both start at `m`. Only the call has a type
    // worth completing against, so take the first that yields a qualifier.
    for (const auto& [candidateLine, candidateColumn] : candidates)
        for (const auto& [expression, _] : analyzer.typeMap()) {
            if (!expression || expression->location.line != candidateLine ||
                expression->location.column != candidateColumn)
                continue;
            if (auto qualifier =
                    completionQualifierForType(analyzer.displayTypeOf(expression));
                !qualifier.empty())
                return qualifier;
        }
    return {};
}

// The receiver's type, cheaply where possible. `recoveredReceiverQualifier`
// re-lexes, re-parses and re-analyzes the WHOLE buffer, which is fine once but
// not per keystroke: hovering in a 900-line file cost ~18ms a request, all of
// it that re-analysis. A receiver that is a plain identifier already has its
// type recorded from the last good analysis, so answer from that map first and
// keep the re-analysis for receivers it cannot describe — a call result, an
// index, a chain.
template <typename Document>
auto receiverQualifier(const Document& document, const DotReceiver& receiver,
                       size_t cursor,
                       const semantic::ImportedInterfaces* interfaces,
                       const std::vector<std::string>& moduleRoots)
    -> std::string {
    if (!receiver.valid) return {};
    // The last good analysis already typed this expression; find it by where
    // it starts.
    const auto spanAt = [&](size_t offset) -> std::string {
        unsigned int line = 1;
        size_t lineStart = 0;
        for (size_t i = 0; i < offset; ++i)
            if (document.text[i] == '\n') { ++line; lineStart = i + 1; }
        auto span = document.receiverSpans.find(Document::receiverKey(
            line, static_cast<unsigned int>(offset - lineStart) + 1));
        return span == document.receiverSpans.end() ? std::string{} : span->second;
    };
    // The call's `(` first: for a chain that is where the OUTERMOST expression
    // is recorded, and its type is the one being completed against. The
    // receiver's start answers the plain `identifier.` case.
    if (receiver.callOpen)
        if (auto qualifier = spanAt(receiver.callOpen); !qualifier.empty())
            return qualifier;
    if (auto qualifier = spanAt(receiver.start); !qualifier.empty())
        return qualifier;


    const auto text = document.text.substr(receiver.start,
                                           receiver.dot - receiver.start);
    // A literal receiver needs no analysis to type: `36.hours` is an Integer
    // with a method on it, and time.kex is full of them. They were the bulk of
    // the misses that fell through to re-analysis.
    if (!text.empty()) {
        if (text.front() == '"') return "String";
        if (std::isdigit(static_cast<unsigned char>(text.front()))) {
            const bool fractional = text.find('.') != std::string::npos;
            return fractional ? "Float" : "Integer";
        }
    }
    if (!text.empty() &&
        text.find_first_of(" \t\n()[]{}.\"'`") == std::string::npos)
        if (auto known = document.localReceiverTypes.find(text);
            known != document.localReceiverTypes.end() && !known->second.empty())
            return known->second;
    // Nothing recorded — the buffer may not have parsed since this expression
    // was written. Re-analyze with the half-typed member removed, once.
    if (auto cached = document.recoveredReceivers.find(receiver.start);
        cached != document.recoveredReceivers.end())
        return cached->second;
    auto recovered = recoveredReceiverQualifier(document.text, document.path,
                                                receiver, cursor, interfaces,
                                                moduleRoots);
    document.recoveredReceivers.insert_or_assign(receiver.start, recovered);
    return recovered;
}

auto recoveredDotReceiverQualifier(
    const std::string& source, const std::string& path,
    std::string_view receiver, unsigned int protocolLine,
    unsigned int protocolCharacter,
    const semantic::ImportedInterfaces* interfaces) -> std::string {
    if (receiver.empty()) return {};
    size_t lineStart = 0;
    for (unsigned int current = 0; current < protocolLine; ++current) {
        const auto newline = source.find('\n', lineStart);
        if (newline == std::string::npos) return {};
        lineStart = newline + 1;
    }
    const auto lineEndPosition = source.find('\n', lineStart);
    const auto lineEnd = lineEndPosition == std::string::npos
        ? source.size() : lineEndPosition;
    const auto cursor = byteOffsetForUtf16Column(
        source, lineStart, lineEnd, protocolCharacter);
    if (!cursor || source[cursor - 1] != '.') return {};

    auto recovered = source;
    recovered.erase(cursor - 1, 1);
    Lexer lexer(recovered, path);
    Parser parser(lexer.tokenizeAll(), path);
    auto program = parser.parseProgram();
    if (!parser.diagnostics().empty()) return {};
    semantic::Analyzer analyzer(interfaces);
    analyzer.analyze(program);
    const auto receiverColumn = static_cast<int>(
        cursor - lineStart - receiver.size() + 1);
    for (const auto& [expression, _] : analyzer.typeMap()) {
        if (!expression ||
            expression->location.line != static_cast<int>(protocolLine + 1) ||
            expression->location.column != receiverColumn)
            continue;
        const auto* identifier = std::get_if<ast::Identifier>(&expression->kind);
        if (!identifier || identifier->name != receiver) continue;
        return completionQualifierForType(analyzer.displayTypeOf(expression));
    }
    return {};
}

void enrichFunctionSymbols(semantic::FileState& state,
                           const semantic::Analyzer& analyzer) {
    auto enrich = [&](const ast::FunctionDef& function) {
        const auto* signatures = analyzer.functionSignatures(&function);
        if (!signatures || signatures->empty()) return;
        for (auto& symbol : state.symbols) {
            if (symbol.name != function.name ||
                symbol.definition.line != function.location.line ||
                symbol.definition.column != function.location.column)
                continue;
            symbol.detail = functionDetail(function, *signatures, analyzer);
            symbol.type = semantic::Type::func(signatures->front().params,
                                               signatures->front().result);
            for (size_t i = 0; i < symbol.params.size() &&
                               i < signatures->front().params.size(); ++i)
                symbol.params[i].second = signatures->front().params[i];
            break;
        }
    };
    walkFunctions(state.ast, enrich);
}

class Server {
public:
    Server(std::istream& input, std::ostream& output,
           const std::string& runtimeBeamDir)
        : m_stream(input, output), m_connection(m_stream),
          m_handler(m_connection, 0),
          m_interfaces(preludeSemanticInterfaces(runtimeBeamDir)),
          m_standardDocumentation(standardLibraryDocumentation()),
          m_standardParameterNames(standardLibraryParameterNames()) {
        m_db.setImportedInterfaces(&m_interfaces);
        m_referenceDb.setImportedInterfaces(&m_interfaces);
        loadDiscoveredPrelude(m_db);
        loadDiscoveredPrelude(m_referenceDb);
        registerHandlers();
    }

    auto run() -> int {
        while (m_running) m_handler.processIncomingMessages();
        return m_cleanShutdown ? 0 : 1;
    }

private:
    struct IndexedCallReference {
        std::string ownerKey;
        SourceLocation location;
    };

    auto initialize(::lsp::InitializeParams&& params)
        -> ::lsp::requests::Initialize::Result {
        if (m_initialized)
            throw ::lsp::RequestError(::lsp::MessageError::InvalidRequest,
                                      "Kex language server is already initialized");
        m_initialized = true;
        if (params.workspaceFolders && !params.workspaceFolders->isNull()) {
            for (const auto& folder : params.workspaceFolders->value())
                if (folder.uri.isFileUri())
                    m_workspaceRoots.push_back(folder.uri.fsPath());
        } else if (!params.rootUri.isNull() && params.rootUri->isFileUri()) {
            m_workspaceRoots.push_back(params.rootUri->fsPath());
        }
        return {
            .capabilities = {
                .positionEncoding = ::lsp::PositionEncodingKind::UTF16,
                .textDocumentSync = ::lsp::TextDocumentSyncOptions{
                    .openClose = true,
                    .change = ::lsp::TextDocumentSyncKind::Full,
                    .save = ::lsp::SaveOptions{.includeText = true},
                },
                .completionProvider = ::lsp::CompletionOptions{
                    .triggerCharacters = ::lsp::Array<::lsp::String>{"."},
                },
                .hoverProvider = true,
                .definitionProvider = true,
                .referencesProvider = true,
            },
            .serverInfo = ::lsp::ServerInfo{
                .name = "kex",
                .version = versionNumber(),
            },
        };
    }

    void verifyInitialized() const {
        if (!m_initialized)
            throw ::lsp::RequestError(::lsp::MessageError::ServerNotInitialized,
                                      "Kex language server is not initialized");
        if (m_cleanShutdown)
            throw ::lsp::RequestError(::lsp::MessageError::InvalidRequest,
                                      "Kex language server has shut down");
    }

    void ensureReferenceIndex(std::string_view requestedName) {
        if ((m_referenceIndexReady && m_referenceIndexWord == requestedName) ||
            m_workspaceRoots.empty())
            return;
        for (const auto& path : m_referenceIndexedPaths)
            m_referenceDb.removeFile(path);
        m_referenceIndexedPaths.clear();
        m_indexedCallReferences.clear();
        m_referenceIndexWord = std::string(requestedName);
        std::vector<std::string> moduleRoots;
        std::vector<std::pair<std::string, std::string>> sources;
        for (const auto& root : m_workspaceRoots) {
            moduleRoots.push_back(root);
            for (const auto* child : {"lib", "src"}) {
                const auto candidate = std::filesystem::path(root) / child;
                std::error_code error;
                if (std::filesystem::is_directory(candidate, error) && !error)
                    moduleRoots.push_back(candidate.string());
            }
        }
        for (const auto& root : standardLibraryModuleRoots())
            if (std::find(moduleRoots.begin(), moduleRoots.end(), root) ==
                moduleRoots.end())
                moduleRoots.push_back(root);
        m_referenceDb.setModuleRoots(std::move(moduleRoots));

        const std::unordered_set<std::string> ignored{
            ".git", ".claude", ".opencode", "build", "build-wasm",
            "node_modules", "third_party", "Testing"};
        std::unordered_set<std::string> preindexedPaths;
        for (const auto& path : standardLibrarySourceFiles())
            preindexedPaths.insert(std::filesystem::path(path).lexically_normal().string());
        for (const auto& path : preludeSourceFiles())
            preindexedPaths.insert(std::filesystem::path(path).lexically_normal().string());
        for (const auto& root : m_workspaceRoots) {
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(
                root, std::filesystem::directory_options::skip_permission_denied,
                error);
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end) {
                const auto entry = *iterator;
                if (entry.is_directory(error) &&
                    ignored.count(entry.path().filename().string())) {
                    iterator.disable_recursion_pending();
                } else if (entry.is_regular_file(error) &&
                           entry.path().extension() == ".kex") {
                    if (preindexedPaths.count(
                            entry.path().lexically_normal().string())) {
                        iterator.increment(error);
                        continue;
                    }
                    std::ifstream input(entry.path(), std::ios::binary);
                    if (input) {
                        std::string source(
                            (std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
                        const auto path = entry.path().string();
                        for (const auto& [_, open] : m_documents)
                            if (open.path == path) {
                                source = open.text;
                                break;
                            }
                        auto containsIdentifier = [&] {
                            auto position = source.find(requestedName);
                            auto identifier = [](unsigned char c) {
                                return std::isalnum(c) || c == '_' ||
                                       c == '?' || c == '!';
                            };
                            while (position != std::string::npos) {
                                const auto after = position + requestedName.size();
                                if ((position == 0 || !identifier(
                                         static_cast<unsigned char>(
                                             source[position - 1]))) &&
                                    (after >= source.size() || !identifier(
                                         static_cast<unsigned char>(
                                             source[after]))))
                                    return true;
                                position = source.find(requestedName,
                                                       position + 1);
                            }
                            return false;
                        };
                        if (!containsIdentifier()) {
                            iterator.increment(error);
                            continue;
                        }
                        sources.emplace_back(path, std::move(source));
                    }
                }
                iterator.increment(error);
            }
        }
        // Collect module declarations before their consumers so references
        // attach to the final indexed SymbolInfo rather than a transient
        // symbol loaded on demand by `using`.
        std::stable_sort(sources.begin(), sources.end(),
            [](const auto& left, const auto& right) {
                const bool leftModule = left.second.find("module ") !=
                    std::string::npos;
                const bool rightModule = right.second.find("module ") !=
                    std::string::npos;
                return leftModule != rightModule ? leftModule :
                    left.first < right.first;
            });
        if (sources.size() > 64) {
            sources.erase(std::remove_if(
                sources.begin(), sources.end(), [&](const auto& candidate) {
                    return std::none_of(
                        m_documents.begin(), m_documents.end(),
                        [&](const auto& open) {
                            return open.second.path == candidate.first;
                        });
                }), sources.end());
        }
        for (auto& [path, source] : sources) {
            m_referenceDb.updateFile(path, std::move(source));
            m_referenceIndexedPaths.push_back(path);
        }
        for (const auto& [path, _] : sources) {
            auto* state = m_referenceDb.fileState(path);
            if (!state) continue;
            semantic::Analyzer analyzer(&m_interfaces);
            analyzer.analyze(state->ast);
            for (const auto& [call, target] : analyzer.resolvedCalls()) {
                if (!call || target.sourceModule.empty()) continue;
                for (const auto& [expression, _] :
                     analyzer.selectedCallSignatures()) {
                    const auto* method = expression
                        ? std::get_if<ast::MethodCall>(&expression->kind)
                        : nullptr;
                    if (method != call) continue;
                    if (auto location = callNameLocation(
                            state->source, *expression, call->method, true))
                        m_indexedCallReferences.push_back({
                            target.sourceModule + "." + call->method,
                            *location,
                        });
                    break;
                }
            }
        }
        m_referenceIndexReady = true;
    }

    void update(const ::lsp::DocumentUri& uri, std::string text, int version) {
        verifyInitialized();
        const auto key = uri.toString();
        auto path = uriPath(uri);
        auto& document = m_documents[key];
        auto previousReceiverTypes = std::move(document.localReceiverTypes);
        document = {path, std::move(text), version};
        document.recoveredReceivers.clear();
        // Keep the last valid receiver types while the user is typing syntax
        // that temporarily cannot be parsed, most notably the trailing dot
        // that triggers member completion.
        document.localReceiverTypes = std::move(previousReceiverTypes);
        document.documentation = sourceDocumentation(document.text);
        m_db.setModuleRoots(moduleRootsFor(path));
        m_db.updateFile(path, document.text, specCompanions(path));
        m_referenceIndexReady = false;
        m_referenceIndexWord.clear();
        publishDiagnostics(uri, document);
    }

    void publishDiagnostics(const ::lsp::DocumentUri& uri,
                            Document& document) {
        std::vector<semantic::Diagnostic> diagnostics(
            m_db.diagnosticsFor(document.path).begin(),
            m_db.diagnosticsFor(document.path).end());
        auto* state = m_db.fileState(document.path);
        if (state) {
            semantic::Analyzer analyzer(&m_interfaces);
            const bool analyzed = analyzer.analyze(state->ast);
            enrichFunctionSymbols(*state, analyzer);
            document.hoverEntries.clear();
            document.selectedCallEntries.clear();
            if (analyzed) {
                document.localReceiverTypes.clear();
                document.receiverSpans.clear();
            }
            for (const auto& [expression, _] : analyzer.typeMap()) {
                if (!expression || expression->location.file != document.path)
                    continue;
                // Every expression that could be completed against, by where
                // it starts. Answering a member completion from this costs a
                // hash lookup; re-deriving it by re-analyzing the buffer cost
                // ~18ms a keystroke in a 900-line file.
                if (auto spanQualifier =
                        completionQualifierForType(analyzer.displayTypeOf(expression));
                    !spanQualifier.empty())
                    document.receiverSpans.insert_or_assign(
                        Document::receiverKey(
                            static_cast<unsigned int>(expression->location.line),
                            static_cast<unsigned int>(expression->location.column)),
                        std::move(spanQualifier));
                if (const auto* binding =
                        std::get_if<ast::LetExpr>(&expression->kind);
                    binding && binding->pattern && binding->value) {
                    if (const auto* variable =
                            std::get_if<ast::VarPattern>(&binding->pattern->kind)) {
                        const auto bindingType =
                            analyzer.displayTypeOf(binding->value.get());
                        if (bindingType && !std::holds_alternative<
                                semantic::UnknownType>(bindingType->kind)) {
                            if (auto qualifier = completionQualifierForType(
                                    bindingType); !qualifier.empty())
                                document.localReceiverTypes[variable->name] =
                                    std::move(qualifier);
                            semantic::Signature display;
                            display.name = variable->name;
                            display.result = bindingType;
                            document.hoverEntries.push_back({
                                .line = static_cast<unsigned int>(
                                    binding->pattern->location.line),
                                .byteColumn = static_cast<unsigned int>(
                                    binding->pattern->location.column),
                                .byteLength = static_cast<unsigned int>(
                                    variable->name.size()),
                                .detail = analyzer.displaySignature(
                                    variable->name, display),
                                .completeDetail = true,
                            });
                        }
                    }
                    // The LetExpr itself has Unit type at the `let` keyword;
                    // that is not a useful hover target.
                    continue;
                }
                const auto type = analyzer.displayTypeOf(expression);
                if (!type || std::holds_alternative<semantic::UnknownType>(type->kind))
                    continue;
                if (const auto* identifier =
                        std::get_if<ast::Identifier>(&expression->kind))
                    if (auto qualifier = completionQualifierForType(type);
                        !qualifier.empty())
                        document.localReceiverTypes[identifier->name] =
                            std::move(qualifier);
                auto location = expression->location;
                auto length = identifierLengthAt(document.text, location);
                semantic::Signature expressionSignature;
                expressionSignature.name = "_";
                expressionSignature.result = type;
                auto renderedType = analyzer.displaySignature("_", expressionSignature);
                const auto typeSeparator = renderedType.find(": ");
                std::string detail = typeSeparator == std::string::npos
                    ? semantic::typeToString(type)
                    : renderedType.substr(typeSeparator + 2);
                bool completeDetail = false;
                if (const auto* identifier =
                        std::get_if<ast::Identifier>(&expression->kind)) {
                    detail = identifier->name + " : " + detail;
                    completeDetail = true;
                }
                if (const auto* curry =
                        std::get_if<ast::CurryExpr>(&expression->kind)) {
                    location.column += curry->isOperator ? 2 :
                        1 + static_cast<int>(curry->module.empty()
                            ? 0 : curry->module.size() + 1);
                    length = static_cast<unsigned int>(curry->name.size());
                    const auto displayName = "~" +
                        (curry->module.empty() ? std::string{} : curry->module + ".") +
                        curry->name;
                    if (const auto* function =
                            std::get_if<semantic::FuncType>(&type->kind)) {
                        semantic::Signature signature;
                        signature.name = displayName;
                        signature.params = function->params;
                        signature.result = function->result;
                        detail = analyzer.displaySignature(displayName, signature);
                    } else {
                        detail = displayName + " : " + semantic::typeToString(type);
                    }
                    completeDetail = true;
                }
                if (const auto* methodCall =
                        std::get_if<ast::MethodCall>(&expression->kind);
                    methodCall && location.line > 0 && location.column > 0) {
                    size_t lineStart = 0;
                    for (int sourceLine = 1; sourceLine < location.line;
                         ++sourceLine) {
                        const auto newline = document.text.find('\n', lineStart);
                        if (newline == std::string::npos) break;
                        lineStart = newline + 1;
                    }
                    const auto offset = lineStart +
                        static_cast<size_t>(location.column - 1);
                    if (offset < document.text.size() &&
                        document.text[offset] == '@') {
                        ++location.column;
                        length = static_cast<unsigned int>(
                            methodCall->method.size());
                        detail = "@" + methodCall->method + " : " + detail;
                        completeDetail = true;
                    }
                }
                if (!length) continue;
                document.hoverEntries.push_back({
                    .line = static_cast<unsigned int>(location.line),
                    .byteColumn = static_cast<unsigned int>(location.column),
                    .byteLength = length,
                    .detail = std::move(detail),
                    .completeDetail = completeDetail,
                });
            }
            // A name a pattern introduces is not an expression, so it has no
            // entry above. Hovering `b` in `Boxed(b)` answered nothing at all
            // until these were recorded.
            for (const auto& binding : analyzer.patternBindings()) {
                if (binding.location.file != document.path || !binding.type)
                    continue;
                if (std::holds_alternative<semantic::UnknownType>(
                        binding.type->kind))
                    continue;
                semantic::Signature display;
                display.name = binding.name;
                display.result = binding.type;
                // The recorded position is the binding's own for a pattern,
                // but only an ANCHOR for a block parameter — `{ |x| … }` marks
                // the block, since a LambdaParam has no location. When the
                // anchor does not sit on the name, find it in the `|…|` list
                // that follows.
                auto line = static_cast<unsigned int>(binding.location.line);
                auto column = static_cast<unsigned int>(binding.location.column);
                if (auto offset = offsetOfLocation(document.text,
                                                   binding.location);
                    offset != std::string::npos &&
                    document.text.compare(offset, binding.name.size(),
                                          binding.name) != 0) {
                    const auto lineEnd = document.text.find('\n', offset);
                    const auto limit = lineEnd == std::string::npos
                        ? document.text.size() : lineEnd;
                    const auto found =
                        document.text.find(binding.name, offset);
                    if (found == std::string::npos || found >= limit) continue;
                    const auto position = locationOfOffset(document.text, found);
                    line = position.first;
                    column = position.second;
                }
                document.hoverEntries.push_back({
                    .line = line,
                    .byteColumn = column,
                    .byteLength = static_cast<unsigned int>(binding.name.size()),
                    .detail = analyzer.displaySignature(binding.name, display),
                    .completeDetail = true,
                });
            }
            for (const auto& [expression, signature] :
                 analyzer.selectedCallSignatures()) {
                if (!expression || expression->location.file != document.path)
                    continue;
                std::string name;
                bool method = false;
                if (const auto* call =
                        std::get_if<ast::FunctionCall>(&expression->kind)) {
                    name = call->name;
                } else if (const auto* call =
                               std::get_if<ast::MethodCall>(&expression->kind)) {
                    name = call->method;
                    method = true;
                } else {
                    continue;
                }
                auto location = callNameLocation(
                    document.text, *expression, name, method);
                if (!location) continue;
                std::string documentationKey;
                if (const auto* methodCall =
                        std::get_if<ast::MethodCall>(&expression->kind))
                    if (auto resolved = analyzer.resolvedCalls().find(methodCall);
                        resolved != analyzer.resolvedCalls().end() &&
                        !resolved->second.sourceModule.empty())
                        documentationKey =
                            resolved->second.sourceModule + "." + name;
                document.selectedCallEntries.push_back({
                    .line = static_cast<unsigned int>(location->line),
                    .byteColumn = static_cast<unsigned int>(location->column),
                    .byteLength = static_cast<unsigned int>(name.size()),
                    .detail = (signature.isFoul ? "foul " : "") +
                              analyzer.displaySignature(name, signature),
                    .documentationKey = std::move(documentationKey),
                    .completeDetail = true,
                });
            }
            diagnostics.insert(diagnostics.end(), analyzer.diagnostics().begin(),
                               analyzer.diagnostics().end());
            const bool hasError = std::any_of(
                diagnostics.begin(), diagnostics.end(), [](const auto& item) {
                    return item.level == semantic::Diagnostic::Level::Error;
                });
            if (analyzed && !hasError) {
                auto validation = validation::validateTaggedLiterals(
                    state->ast, analyzer, moduleRootsFor(document.path));
                diagnostics.insert(diagnostics.end(),
                                   std::make_move_iterator(validation.begin()),
                                   std::make_move_iterator(validation.end()));
            }
        }

        ::lsp::Array<::lsp::Diagnostic> items;
        std::unordered_set<std::string> seen;
        for (const auto& diagnostic : diagnostics) {
            const auto key = std::to_string(diagnostic.location.line) + ":" +
                             std::to_string(diagnostic.location.column) + ":" +
                             diagnostic.message;
            if (!seen.insert(key).second) continue;
            const std::string* diagnosticSource =
                diagnostic.location.file == document.path ? &document.text : nullptr;
            ::lsp::Diagnostic item{
                .range = {
                    .start = protocolPosition(diagnostic.location, diagnosticSource),
                    .end = diagnostic.endLocation
                               ? protocolPosition(*diagnostic.endLocation, diagnosticSource)
                               : pointEnd(diagnostic.location, diagnosticSource),
                },
                .message = diagnostic.message,
                .severity = diagnostic.level == semantic::Diagnostic::Level::Error
                                ? ::lsp::DiagnosticSeverity::Error
                                : ::lsp::DiagnosticSeverity::Warning,
                .source = "kex",
            };
            if (!diagnostic.notes.empty()) {
                ::lsp::Array<::lsp::DiagnosticRelatedInformation> related;
                for (const auto& note : diagnostic.notes) {
                    related.push_back({
                        .location = {
                            .uri = ::lsp::Uri::fileUriFromPath(note.location.file),
                            .range = {
                                .start = protocolPosition(note.location),
                                .end = pointEnd(note.location),
                            },
                        },
                        .message = note.message,
                    });
                }
                item.relatedInformation = std::move(related);
            }
            items.push_back(std::move(item));
        }
        m_handler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>({
            .uri = uri,
            .diagnostics = std::move(items),
            .version = document.version,
        });
    }

    auto completions(::lsp::CompletionParams&& params)
        -> ::lsp::requests::TextDocument_Completion::Result {
        verifyInitialized();
        const auto key = params.textDocument.uri.toString();
        const auto found = m_documents.find(key);
        if (found == m_documents.end()) return ::lsp::Array<::lsp::CompletionItem>{};
        const auto prefix = completionPrefix(found->second.text,
                                             params.position.line,
                                             params.position.character);
        auto query = resolveCompletionQuery(prefix.c_str(), 0, prefix.c_str());

        // Ask the analyzer what the receiver actually is, before falling back
        // to the text heuristic `resolveCompletionQuery` inherited from the
        // REPL. That heuristic reads one line and stops at `)`, so it answers
        // `.g` for `makeBox(3).g` and for a chain continued on the next line,
        // and answers `List` for `[c][0]` — the type of the wrong expression.
        bool resolvedByAnalyzer = false;
        {
            const auto& text = found->second.text;
            size_t lineStart = 0;
            bool haveLine = true;
            for (unsigned int current = 0; current < params.position.line;
                 ++current) {
                const auto newline = text.find('\n', lineStart);
                if (newline == std::string::npos) { haveLine = false; break; }
                lineStart = newline + 1;
            }
            if (haveLine) {
                const auto lineEndPosition = text.find('\n', lineStart);
                const auto lineEnd = lineEndPosition == std::string::npos
                    ? text.size() : lineEndPosition;
                const auto cursor = byteOffsetForUtf16Column(
                    text, lineStart, lineEnd, params.position.character);
                if (const auto receiver = scanDotReceiver(text, cursor);
                    receiver.valid) {
                    if (auto qualifier = receiverQualifier(
                            found->second, receiver, cursor, &m_interfaces,
                            moduleRootsFor(found->second.path));
                        !qualifier.empty()) {
                        query.dbQuery = qualifier + "." +
                            text.substr(receiver.dot + 1,
                                        cursor - receiver.dot - 1);
                        query.rewriteFrom = qualifier;
                        query.rewriteTo = text.substr(
                            receiver.start, receiver.dot - receiver.start);
                        resolvedByAnalyzer = true;
                    }
                }
            }
        }

        if (const auto dot = prefix.find('.');
            !resolvedByAnalyzer && dot != std::string::npos) {
            const auto receiver = prefix.substr(0, dot);
            auto inferredQualifier = [&]() -> std::string {
                if (auto inferred = found->second.localReceiverTypes.find(receiver);
                    inferred != found->second.localReceiverTypes.end())
                    return inferred->second;
                size_t lineStart = 0;
                for (unsigned int current = 0; current < params.position.line;
                     ++current) {
                    const auto newline = found->second.text.find('\n', lineStart);
                    if (newline == std::string::npos) break;
                    lineStart = newline + 1;
                }
                return sourceLocalReceiverQualifier(
                    found->second.text, receiver, lineStart);
            }();
            if (inferredQualifier.empty() && prefix.ends_with('.'))
                inferredQualifier = recoveredDotReceiverQualifier(
                    found->second.text, found->second.path, receiver,
                    params.position.line, params.position.character,
                    &m_interfaces);
            if (!inferredQualifier.empty()) {
                query.dbQuery = inferredQualifier + prefix.substr(dot);
                query.rewriteFrom = inferredQualifier;
                query.rewriteTo = receiver;
            }
        }
        auto matches = rewriteCompletions(
            m_db.completionsAt(found->second.path, params.position.line + 1,
                               completionByteColumn(found->second.text,
                                                    params.position.line,
                                                    params.position.character),
                               query.dbQuery),
            query.rewriteFrom, query.rewriteTo);
        ::lsp::Array<::lsp::CompletionItem> result;
        for (auto& match : matches) {
            const auto separator = match.rfind('.');
            auto leaf = match.substr(separator == std::string::npos ? 0 : separator + 1);
            std::vector<const semantic::ImportedFunction*> functions;
            const auto qualifier = separator == std::string::npos
                ? std::string{} : match.substr(0, separator);
            if (auto module = m_interfaces.modules.find(qualifier);
                !qualifier.empty() && module != m_interfaces.modules.end()) {
                if (auto exports = module->second.exports.find(leaf);
                    exports != module->second.exports.end())
                    for (const auto& function : exports->second)
                        functions.push_back(&function);
            } else if (auto receivers = m_interfaces.receiverFunctions.find(leaf);
                       receivers != m_interfaces.receiverFunctions.end()) {
                for (const auto& function : receivers->second)
                    functions.push_back(&function);
            }
            if (functions.empty() && qualifier.empty())
                for (const auto& [_, module] : m_interfaces.modules) {
                    if (!module.automaticImport) continue;
                    if (auto exports = module.exports.find(leaf);
                        exports != module.exports.end())
                        for (const auto& function : exports->second)
                            functions.push_back(&function);
                }

            semantic::TypeChecker renderer(&m_interfaces);
            std::vector<std::string> signatures;
            for (const auto* function : functions) {
                auto rendered = renderer.displaySignature(leaf, function->signature);
                if (function->signature.isFoul) rendered = "foul " + rendered;
                if (std::find(signatures.begin(), signatures.end(), rendered) ==
                    signatures.end())
                    signatures.push_back(std::move(rendered));
            }
            const auto* localSymbol = m_db.findSymbol(
                leaf, found->second.path);
            if (signatures.empty() && localSymbol &&
                !localSymbol->detail.empty()) {
                std::istringstream details(localSymbol->detail);
                std::string signature;
                while (std::getline(details, signature))
                    if (!signature.empty()) signatures.push_back(signature);
            }
            std::string signatureDetail;
            for (const auto& signature : signatures) {
                if (!signatureDetail.empty()) signatureDetail += '\n';
                signatureDetail += signature;
            }
            std::optional<::lsp::CompletionItemLabelDetails> labelDetails;
            if (!signatures.empty()) {
                auto suffix = signatures.front();
                const bool foulSignature = suffix.rfind("foul ", 0) == 0;
                if (foulSignature) suffix.erase(0, 5);
                const auto name = suffix.find(leaf + " :");
                if (name != std::string::npos)
                    suffix.erase(name, leaf.size());
                std::vector<std::string> parameterNames;
                if (!functions.empty())
                    parameterNames = functions.front()->paramNames;
                else if (localSymbol)
                    for (const auto& [name, _] : localSymbol->params)
                        parameterNames.push_back(name);
                if (parameterNames.empty())
                    if (auto names = m_standardParameterNames.find(leaf);
                        names != m_standardParameterNames.end() &&
                        !names->second.empty())
                        parameterNames = names->second.front();
                if (!parameterNames.empty()) {
                    std::string names = "(";
                    for (size_t i = 0; i < parameterNames.size(); ++i) {
                        if (i) names += ", ";
                        names += parameterNames[i];
                    }
                    names += ")";
                    suffix = names + suffix;
                }
                if (signatures.size() > 1)
                    suffix += " (+" + std::to_string(signatures.size() - 1) +
                        " overloads)";
                // VS Code renders labelDetails.detail directly after the label
                // with no spacing, so a suffix starting with a word runs into
                // it: `ParsedOptionsrecord ParsedOptions do`. Parameter lists
                // start with `(` and read correctly as they are.
                if (!suffix.empty() && suffix.front() != ' ' &&
                    suffix.front() != '(')
                    suffix.insert(0, " ");
                labelDetails = ::lsp::CompletionItemLabelDetails{
                    .detail = std::move(suffix),
                    .description = qualifier.empty()
                        ? std::optional<::lsp::String>{}
                        : std::optional<::lsp::String>{qualifier},
                };
            }
            const bool foulCompletion = !signatures.empty() &&
                signatures.front().rfind("foul ", 0) == 0;
            ::lsp::CompletionItem item{
                .label = foulCompletion ? "foul " + leaf : leaf,
                .labelDetails = std::move(labelDetails),
                .kind = !leaf.empty() &&
                                std::isupper(static_cast<unsigned char>(leaf.front()))
                            ? ::lsp::CompletionItemKind::Class
                            : ::lsp::CompletionItemKind::Function,
                .detail = signatureDetail.empty() ? match : signatureDetail,
                .filterText = leaf,
                .insertText = std::move(leaf),
            };
            const std::vector<std::string>* completionDocs = nullptr;
            const auto documentationName = foulCompletion
                ? item.label.substr(5) : item.label;
            if (auto docs = found->second.documentation.find(documentationName);
                docs != found->second.documentation.end())
                completionDocs = &docs->second;
            else {
                const auto standardName =
                    !qualifier.empty() && m_interfaces.modules.count(qualifier)
                        ? qualifier + "." + documentationName
                        : documentationName;
                if (auto docs = m_standardDocumentation.find(standardName);
                    docs != m_standardDocumentation.end())
                    completionDocs = &docs->second;
            }
            if (completionDocs) {
                std::string documentation;
                for (const auto& entry : *completionDocs) {
                    if (!documentation.empty()) documentation += "\n\n";
                    documentation += entry;
                }
                item.documentation = ::lsp::OneOf<::lsp::String,
                    ::lsp::MarkupContent>{::lsp::MarkupContent{
                        .kind = ::lsp::MarkupKind::Markdown,
                        .value = renderRdoc(std::move(documentation)),
                    }};
            }
            // Methods the RECEIVER'S OWN type declares come first. A client
            // sorts by sortText when it is set and alphabetically when it is
            // not, so `box.` used to open on whatever prelude method happened
            // to sort earliest rather than on `make Box`'s own methods.
            // `query.rewriteFrom` is the receiver type this completion
            // resolved to, and `qualifier` is where each match comes from.
            // The qualifier cannot answer this: rewriteCompletions has already
            // turned every match's `Box.` into the receiver text, so they all
            // look alike by then. `makeTarget` is the `make TypeName` a method
            // was declared in, which is exactly the question being asked.
            if (!query.rewriteFrom.empty() && localSymbol)
                item.sortText =
                    (localSymbol->makeTarget == query.rewriteFrom ? "0" : "1") +
                    item.label;
            else if (!query.rewriteFrom.empty())
                item.sortText = "1" + item.label;
            result.push_back(std::move(item));
        }
        return result;
    }

    auto hover(::lsp::HoverParams&& params)
        -> ::lsp::requests::TextDocument_Hover::Result {
        verifyInitialized();
        const auto key = params.textDocument.uri.toString();
        const auto found = m_documents.find(key);
        if (found == m_documents.end()) return {};
        const auto& document = found->second;
        const auto word = wordAt(document.text, params.position.line,
                                 params.position.character);
        if (word.text.empty()) return {};
        // `xs.push!(v)` is the mutating form of `push`, produced by the
        // language rather than declared anywhere: no symbol, no signature and
        // no documentation is ever registered under `push!`, so every lookup
        // below has to ask about the base name. The bang is kept for display.
        std::string lookupName = word.text;
        if (lookupName.size() > 1 && lookupName.back() == '!' &&
            !m_db.findSymbol(lookupName, document.path))
            lookupName.pop_back();
        const auto moduleQualifier = moduleQualifierBeforeWord(
            document.text, params.position.line, word.byteColumn);

        const semantic::SymbolInfo* symbol = m_db.symbolAt(
            document.path, params.position.line + 1, word.byteColumn);
        const bool resolvedSymbolAtPosition = symbol != nullptr;
        size_t hoverLineStart = 0;
        for (unsigned int current = 0; current < params.position.line;
             ++current) {
            const auto newline = document.text.find('\n', hoverLineStart);
            if (newline == std::string::npos) break;
            hoverLineStart = newline + 1;
        }
        const auto hoverWordOffset = hoverLineStart + word.byteColumn - 1;
        const auto localDefinition =
            (!resolvedSymbolAtPosition && moduleQualifier.empty())
                ? sourceLocalDefinition(
                      document.text, document.path, word.text,
                      hoverWordOffset ? hoverWordOffset - 1 : 0)
                : std::nullopt;
        const bool lexicalReference = localDefinition.has_value();
        const bool symbolDefinitionAtPosition = symbol &&
            symbol->definition.file == document.path &&
            symbol->definition.line == static_cast<int>(params.position.line + 1) &&
            word.byteColumn >= static_cast<uint32_t>(symbol->definition.column) &&
            word.byteColumn < static_cast<uint32_t>(symbol->definition.column) +
                                  symbol->name.size();
        if (!symbol && moduleQualifier.empty())
            symbol = m_db.findSymbol(lookupName, document.path);

        std::string detail;
        std::string documentation;
        std::string selectedCallDetail;
        // `using FS` names a MODULE, and nothing downstream knows that: the
        // symbol search resolved `FS` to whatever else carried the name and
        // hovering an import reported an unrelated declaration from the same
        // file. On a `using` line the word can only be a module.
        // Anything on a `using` line is part of a module path by
        // construction, qualified (`using Units.SI`) or not, so no lookup can
        // make it something else.
        const bool importLine =
            document.text.compare(hoverLineStart, 6, "using ") == 0;
        semantic::TypeChecker traitLookup(&m_interfaces);
        const bool traitName = traitLookup.isTrait(lookupName) ||
            (symbol && symbol->kind == semantic::SymbolKind::Trait);
        const auto* importedAdt = moduleQualifier.empty()
            ? importedAdtNamed(m_interfaces, lookupName) : nullptr;
        const auto* constructorAdt = moduleQualifier.empty()
            ? importedAdtWithConstructor(m_interfaces, lookupName) : nullptr;
        const Document::HoverEntry* expressionHover = nullptr;
        const Document::HoverEntry* selectedCallHover = nullptr;
        const auto line = params.position.line + 1;
        for (const auto& entry : document.hoverEntries) {
            if (entry.line == line && word.byteColumn >= entry.byteColumn &&
                word.byteColumn < entry.byteColumn + entry.byteLength &&
                (!expressionHover || entry.completeDetail)) {
                expressionHover = &entry;
                if (entry.completeDetail) break;
            }
        }
        for (const auto& entry : document.selectedCallEntries)
            if (entry.line == line && word.byteColumn >= entry.byteColumn &&
                word.byteColumn < entry.byteColumn + entry.byteLength) {
                selectedCallHover = &entry;
                break;
            }
        const bool primitiveTypeReference =
            semantic::isPrimitiveTypeName(word.text) &&
            (isTypeAnnotationPosition(document.text, params.position.line,
                                      word.byteColumn) ||
             !isNamespaceReceiverPosition(document.text, params.position.line,
                                          word.byteColumn, word.byteLength));
        const bool typeReference = isTypeAnnotationPosition(
            document.text, params.position.line, word.byteColumn);
        const auto shorthandFieldType = atFieldType(
            document.text, params.position.line, word.byteColumn, word.text,
            m_db, document.path);
        // A field of the record the receiver actually has. Resolved through
        // the same analyzer-backed receiver lookup completion uses, so it
        // works for `b.size`, `makeBox(3).size`, and a match binding alike.
        std::string fieldDetail;
        if (moduleQualifier.empty() && !importLine) {
            const auto wordEnd = hoverLineStart + word.byteColumn - 1 +
                                 word.byteLength;
            if (const auto receiver = scanDotReceiver(document.text, wordEnd);
                receiver.valid)
                if (auto qualifier = receiverQualifier(
                        document, receiver, wordEnd, &m_interfaces,
                        moduleRootsFor(document.path));
                    !qualifier.empty())
                    if (const auto* record =
                            m_db.findSymbol(qualifier, document.path);
                        record && record->kind == semantic::SymbolKind::Record)
                        if (auto type = recordFieldType(record->detail, lookupName);
                            !type.empty())
                            fieldDetail = word.text + " : " + type;
        }

        // Documentation is looked up by BARE NAME, so a field must not take
        // it: `user.name` showed the right type and then OptionParser's prose
        // about `kex install`, because that module happens to export a `name`.
        const bool mapKey = isMapKeyPosition(document.text,
                                             params.position.line + 1,
                                             word.byteColumn, word.byteLength);
        const bool recordField = !fieldDetail.empty();
        const auto fieldDeclaration = mapKey
            ? std::string{}
            : declaredFieldType(document.text, params.position.line + 1,
                                word.byteColumn, word.byteLength);
        if (mapKey) {
            // In a RECORD literal the key is that record's field, so it has a
            // declared type; in a map literal it is genuinely an atom.
            detail.clear();
            if (const auto record = literalRecordName(
                    document.text, params.position.line + 1, word.byteColumn);
                !record.empty())
                if (const auto* declaration =
                        m_db.findSymbol(record, document.path);
                    declaration &&
                    declaration->kind == semantic::SymbolKind::Record)
                    if (auto type = recordFieldType(declaration->detail,
                                                    word.text);
                        !type.empty())
                        detail = word.text + " : " + type;
            if (detail.empty()) detail = word.text + " : Atom";
            symbol = nullptr;
        } else if (!fieldDeclaration.empty()) {
            detail = word.text + " : " + fieldDeclaration;
            symbol = nullptr;
        } else if (recordField) {
            detail = std::move(fieldDetail);
            symbol = nullptr;
        } else if (importLine) {
            // Hovering `FS` in `using FS` used to report an unrelated
            // declaration that happened to share the name — `type FilePath =
            // String` from the same file, or a `Feature` type — because the
            // branches below answer for values and types, and an import is
            // neither.
            detail = "module " + (moduleQualifier.empty()
                                      ? word.text
                                      : moduleQualifier + "." + word.text);
            symbol = nullptr;
        } else if (!shorthandFieldType.empty()) {
            detail = "@" + word.text + " : " + shorthandFieldType;
            symbol = nullptr;
        } else if (primitiveTypeReference) {
            detail = "type " + word.text;
            if (traitName) detail += "\ntrait " + word.text;
            symbol = nullptr;
        } else if (traitName && moduleQualifier.empty()) {
            detail = "type " + word.text + "\ntrait " + word.text;
        } else if (symbol && symbol->kind == semantic::SymbolKind::Type &&
                   symbol->detail.rfind("type ", 0) == 0 &&
                   (symbolDefinitionAtPosition || typeReference)) {
            detail = symbol->detail;
            if (const auto* definitionState =
                    m_db.fileState(std::string(symbol->definition.file)))
                documentation = documentationBefore(*definitionState,
                                                     symbol->definition);
        } else if (symbol && symbol->kind == semantic::SymbolKind::Record &&
                   symbol->detail.rfind("record ", 0) == 0) {
            detail = symbol->detail;
            if (const auto* definitionState =
                    m_db.fileState(std::string(symbol->definition.file)))
                documentation = documentationBefore(*definitionState,
                                                     symbol->definition);
        } else if (importedAdt && typeReference) {
            detail = importedAdtDetail(*importedAdt);
        } else if (expressionHover &&
                   (expressionHover->completeDetail ||
                    (!selectedCallHover && moduleQualifier.empty()) ||
                    (!word.text.empty() && std::isupper(
                        static_cast<unsigned char>(word.text.front()))))) {
            detail = expressionHover->completeDetail
                ? expressionHover->detail
                : word.text + " : " + expressionHover->detail;
        } else if (symbol && resolvedSymbolAtPosition) {
            detail = symbol->detail;
            if (detail.empty() && symbol->type &&
                !std::holds_alternative<semantic::UnknownType>(symbol->type->kind))
                detail = symbol->name + " : " + semantic::typeToString(symbol->type);
            if (const auto* definitionState =
                    m_db.fileState(std::string(symbol->definition.file)))
                documentation = documentationBefore(*definitionState,
                                                     symbol->definition);
        } else if (constructorAdt) {
            detail = importedConstructorDetail(*constructorAdt, word.text);
        } else if (symbol) {
            detail = symbol->detail;
            if (detail.empty() && symbol->type &&
                !std::holds_alternative<semantic::UnknownType>(symbol->type->kind))
                detail = symbol->name + " : " + semantic::typeToString(symbol->type);
            if (const auto* definitionState =
                    m_db.fileState(std::string(symbol->definition.file)))
                documentation = documentationBefore(*definitionState,
                                                     symbol->definition);
        }

        if (detail.empty()) {
            if (constructorAdt)
                detail = importedConstructorDetail(*constructorAdt, word.text);
        }

        // A LOCAL binding shadows anything imported under the same name, so
        // its signatures are not an answer about this word. `let name =
        // json["name"].or(unknown)` hovered as `name : Weekday -> String`,
        // with OptionParser's prose about `kex install` attached, because a
        // module exports a `name` and the imported lookup ran first.
        if (detail.empty() && !lexicalReference) {
            std::vector<std::pair<semantic::Signature, bool>> importedSignatures;
            if (!moduleQualifier.empty()) {
                if (auto module = m_interfaces.modules.find(moduleQualifier);
                    module != m_interfaces.modules.end())
                    if (auto functions = module->second.exports.find(lookupName);
                        functions != module->second.exports.end())
                        for (const auto& function : functions->second)
                            importedSignatures.emplace_back(
                                function.signature,
                                function.signature.isFoul);
            } else {
                // Modules this file opted into with `using`, as well as the
                // automatic ones. Without the former, hovering `meter` in
                // `100.meter` found nothing — `Units.SI` is opt-in, so its
                // exports were skipped and the reply fell through to the
                // bare string "function meter".
                const auto opened = documentImports(found->second.text);
                for (const auto& [moduleName, module] : m_interfaces.modules) {
                    if (!module.automaticImport && !opened.count(moduleName))
                        continue;
                    if (auto functions = module.exports.find(lookupName);
                        functions != module.exports.end())
                        for (const auto& function : functions->second)
                            importedSignatures.emplace_back(
                                function.signature,
                                function.signature.isFoul);
                }
                if (auto functions = m_interfaces.receiverFunctions.find(lookupName);
                    functions != m_interfaces.receiverFunctions.end())
                    for (const auto& function : functions->second) {
                        importedSignatures.emplace_back(function.signature,
                                                       function.signature.isFoul);
                    }
            }
            semantic::TypeChecker renderer(&m_interfaces);
            std::unordered_set<std::string> rendered;
            for (const auto& [signature, isFoul] : importedSignatures) {
                auto lineDetail = renderer.displaySignature(lookupName, signature);
                if (isFoul) lineDetail = "foul " + lineDetail;
                if (!rendered.insert(lineDetail).second) continue;
                if (!detail.empty()) detail += '\n';
                detail += std::move(lineDetail);
            }
            if (!importedSignatures.empty()) {
                const auto standardName = moduleQualifier.empty()
                    ? lookupName : moduleQualifier + "." + lookupName;
                if (auto docs = m_standardDocumentation.find(standardName);
                    docs != m_standardDocumentation.end())
                    for (const auto& entry : docs->second) {
                        if (documentation.find(entry) != std::string::npos)
                            continue;
                        if (!documentation.empty()) documentation += "\n\n";
                        documentation += entry;
                    }
            }
        }
        // Whether what is being shown came from a DECLARATION of this name
        // rather than from the type of the expression sitting here. A map
        // key, a local, a pattern binding: their type is the answer, but they
        // declare nothing, so documentation must not be attached by name — a
        // key called `name` was picking up OptionParser's prose about
        // `kex install`.
        bool detailFromExpression = false;
        if (detail.empty()) {
            if (expressionHover) {
                detail = expressionHover->completeDetail
                    ? expressionHover->detail
                    : word.text + " : " + expressionHover->detail;
                detailFromExpression = true;
                // Anything gathered above belonged to a same-named DECLARATION
                // that this expression is not. The lookups run before the
                // answer is known, so the prose has to be dropped here rather
                // than skipped there.
                documentation.clear();
            }
        }
        // Nothing typed this occurrence — an identifier inside a string
        // interpolation (`"${name}"`) never reaches the type map at all — but
        // its local definition was found, and THAT was typed. Answer with what
        // the binding says rather than the bare word "function name".
        if (detail.empty() && localDefinition)
            for (const auto& entry : document.hoverEntries)
                if (entry.line ==
                        static_cast<unsigned int>(localDefinition->line) &&
                    entry.byteColumn ==
                        static_cast<unsigned int>(localDefinition->column)) {
                    detail = entry.completeDetail
                        ? entry.detail
                        : word.text + " : " + entry.detail;
                    break;
                }
        if (detail.empty() && symbol) {
            const char* kind = "symbol";
            if (symbol->kind == semantic::SymbolKind::Module) kind = "module";
            else if (symbol->kind == semantic::SymbolKind::Type) kind = "type";
            else if (symbol->kind == semantic::SymbolKind::Trait) kind = "trait";
            else if (symbol->kind == semantic::SymbolKind::Record) kind = "record";
            else if (symbol->kind == semantic::SymbolKind::Function) kind = "function";
            detail = std::string(kind) + " " + symbol->name;
        }
        if (selectedCallHover) {
            selectedCallDetail = selectedCallHover->detail;
            std::string remaining;
            std::istringstream existing(detail);
            std::string overload;
            while (std::getline(existing, overload)) {
                if (overload.empty() || overload == selectedCallHover->detail)
                    continue;
                if (!remaining.empty()) remaining += '\n';
                remaining += overload;
            }
            detail = std::move(remaining);
            if (!selectedCallHover->documentationKey.empty())
                if (auto docs = m_standardDocumentation.find(
                        selectedCallHover->documentationKey);
                    docs != m_standardDocumentation.end()) {
                    documentation.clear();
                    for (const auto& entry : docs->second) {
                        if (!documentation.empty()) documentation += "\n\n";
                        documentation += entry;
                    }
                }
        }
        if (documentation.empty() && !recordField && !mapKey &&
            fieldDeclaration.empty() && !detailFromExpression)
            if (auto docs = document.documentation.find(lookupName);
                docs != document.documentation.end())
                for (const auto& entry : docs->second) {
                    if (!documentation.empty()) documentation += "\n\n";
                    documentation += entry;
                }
        if (documentation.empty() && !lexicalReference && !recordField &&
            !mapKey && fieldDeclaration.empty() && !detailFromExpression) {
            const auto standardName = moduleQualifier.empty()
                ? lookupName : moduleQualifier + "." + lookupName;
            if (auto docs = m_standardDocumentation.find(standardName);
                docs != m_standardDocumentation.end())
                for (const auto& entry : docs->second) {
                    if (!documentation.empty()) documentation += "\n\n";
                    documentation += entry;
                }
        }
        if (detail.empty() && selectedCallDetail.empty()) return {};

        std::string markdown;
        if (!selectedCallDetail.empty()) {
            markdown = "**Selected overload**\n\n```kex\n" +
                       selectedCallDetail + "\n```";
            if (!detail.empty())
                markdown += "\n\n**Other overloads**\n\n```kex\n" +
                            detail + "\n```";
        } else {
            markdown = "```kex\n" + detail + "\n```";
        }
        // `push!` is not a function: the `!` is a marker that rebinds the
        // receiver, and what it calls is `push`. Signatures above are rendered
        // under that real name, so say where the bang went.
        if (lookupName != word.text)
            markdown += "\n\n`" + word.text + "` calls `" + lookupName +
                        "` and rebinds the receiver.";
        if (!documentation.empty()) markdown += "\n\n" + renderRdoc(documentation);
        const SourceLocation start{document.path,
                                   static_cast<int>(params.position.line + 1),
                                   static_cast<int>(word.byteColumn)};
        const SourceLocation end{document.path,
                                 static_cast<int>(params.position.line + 1),
                                 static_cast<int>(word.byteColumn + word.byteLength)};
        return ::lsp::Hover{
            .contents = ::lsp::MarkupContent{
                .kind = ::lsp::MarkupKind::Markdown,
                .value = std::move(markdown),
            },
            .range = ::lsp::Range{
                .start = protocolPosition(start, &document.text),
                .end = protocolPosition(end, &document.text),
            },
        };
    }

    auto definition(::lsp::DefinitionParams&& params)
        -> ::lsp::requests::TextDocument_Definition::Result {
        verifyInitialized();
        const auto key = params.textDocument.uri.toString();
        const auto found = m_documents.find(key);
        if (found == m_documents.end()) return {};
        const auto& document = found->second;
        const auto word = wordAt(document.text, params.position.line,
                                 params.position.character);
        if (word.text.empty()) return {};
        const auto qualifier = moduleQualifierBeforeWord(
            document.text, params.position.line, word.byteColumn);
        const semantic::SymbolInfo* symbol = m_db.symbolAt(
            document.path, params.position.line + 1, word.byteColumn);
        if (!qualifier.empty()) {
            symbol = m_db.symbolInModule(qualifier, word.text);
            // Source collection currently stores a nested module under its
            // local name (`File`) while imported interfaces expose its fully
            // qualified identity (`FS.File`). Keep navigation useful until
            // those indexes share one canonical module key.
            if (!symbol)
                if (const auto dot = qualifier.rfind('.');
                    dot != std::string::npos)
                    symbol = m_db.symbolInModule(
                        qualifier.substr(dot + 1), word.text);
        }
        if (!symbol && qualifier.empty()) {
            size_t lineStart = 0;
            for (unsigned int current = 0; current < params.position.line;
                 ++current) {
                const auto newline = document.text.find('\n', lineStart);
                if (newline == std::string::npos) break;
                lineStart = newline + 1;
            }
            const auto referenceOffset = lineStart + word.byteColumn - 1;
            if (auto local = sourceLocalDefinition(
                    document.text, document.path, word.text,
                    referenceOffset ? referenceOffset - 1 : 0)) {
                const auto start = protocolPosition(*local, &document.text);
                auto endLocation = *local;
                endLocation.column += static_cast<int>(word.text.size());
                return ::lsp::Definition{::lsp::Location{
                    .uri = ::lsp::Uri::fileUriFromPath(document.path),
                    .range = {
                        .start = start,
                        .end = protocolPosition(endLocation, &document.text),
                    },
                }};
            }
            symbol = m_db.findSymbol(word.text, document.path);
        }
        if (!symbol || symbol->definition.file.empty()) return {};
        const auto* state = m_db.fileState(
            std::string(symbol->definition.file));
        const std::string* source = state ? &state->source : nullptr;
        const auto start = protocolPosition(symbol->definition, source);
        auto endLocation = symbol->definition;
        endLocation.column += static_cast<int>(symbol->name.size());
        const auto end = protocolPosition(endLocation, source);
        return ::lsp::Definition{::lsp::Location{
            .uri = ::lsp::Uri::fileUriFromPath(symbol->definition.file),
            .range = {.start = start, .end = end},
        }};
    }

    auto references(::lsp::ReferenceParams&& params)
        -> ::lsp::requests::TextDocument_References::Result {
        verifyInitialized();
        const auto key = params.textDocument.uri.toString();
        const auto found = m_documents.find(key);
        if (found == m_documents.end()) return ::lsp::Array<::lsp::Location>{};
        const auto& document = found->second;
        const auto word = wordAt(document.text, params.position.line,
                                 params.position.character);
        if (word.text.empty()) return ::lsp::Array<::lsp::Location>{};

        auto locationFor = [&](const SourceLocation& location,
                               std::string_view name) -> ::lsp::Location {
            const auto* state = m_db.fileState(std::string(location.file));
            if (!state)
                state = m_referenceDb.fileState(std::string(location.file));
            const std::string* source = state ? &state->source : nullptr;
            auto end = location;
            end.column += static_cast<int>(name.size());
            return {
                .uri = ::lsp::Uri::fileUriFromPath(location.file),
                .range = {
                    .start = protocolPosition(location, source),
                    .end = protocolPosition(end, source),
                },
            };
        };
        auto sameLocation = [](const SourceLocation& left,
                               const SourceLocation& right) {
            return left.file == right.file && left.line == right.line &&
                   left.column == right.column;
        };

        const auto qualifier = moduleQualifierBeforeWord(
            document.text, params.position.line, word.byteColumn);
        const semantic::SymbolInfo* symbol = m_db.symbolAt(
            document.path, params.position.line + 1, word.byteColumn);
        if (!qualifier.empty()) {
            symbol = m_db.symbolInModule(qualifier, word.text);
            if (!symbol)
                if (const auto dot = qualifier.rfind('.');
                    dot != std::string::npos)
                    symbol = m_db.symbolInModule(
                        qualifier.substr(dot + 1), word.text);
        }

        ensureReferenceIndex(word.text);
        if (m_referenceIndexReady) {
            const semantic::SymbolInfo* indexed = nullptr;
            if (symbol) {
                if (!symbol->module.empty())
                    indexed = m_referenceDb.symbolInModule(
                        symbol->module, symbol->name);
                else if (!symbol->makeTarget.empty())
                    indexed = m_referenceDb.receiverSymbol(
                        symbol->makeTarget, symbol->name);
                else
                    indexed = m_referenceDb.findSymbol(
                        symbol->name, std::string(symbol->definition.file));
            } else if (!qualifier.empty()) {
                indexed = m_referenceDb.symbolInModule(qualifier, word.text);
                if (!indexed)
                    if (const auto dot = qualifier.rfind('.');
                        dot != std::string::npos)
                        indexed = m_referenceDb.symbolInModule(
                            qualifier.substr(dot + 1), word.text);
            }
            if (indexed) symbol = indexed;
        }

        ::lsp::Array<::lsp::Location> result;
        std::unordered_set<std::string> seen;
        auto append = [&](const SourceLocation& location) {
            const auto identity = std::string(location.file) + ":" +
                std::to_string(location.line) + ":" +
                std::to_string(location.column);
            if (seen.insert(identity).second)
                result.push_back(locationFor(location, word.text));
        };
        auto spellsWord = [&](const SourceLocation& location) {
            const auto* state = m_db.fileState(std::string(location.file));
            if (!state)
                state = m_referenceDb.fileState(std::string(location.file));
            if (!state || location.line < 1 || location.column < 1) return false;
            size_t offset = 0;
            for (int line = 1; line < location.line; ++line) {
                const auto newline = state->source.find('\n', offset);
                if (newline == std::string::npos) return false;
                offset = newline + 1;
            }
            offset += static_cast<size_t>(location.column - 1);
            return offset + word.text.size() <= state->source.size() &&
                   state->source.compare(offset, word.text.size(), word.text) == 0;
        };
        if (symbol) {
            if (params.context.includeDeclaration) append(symbol->definition);
            for (const auto& reference : symbol->references)
                if (spellsWord(reference)) append(reference);

            // Qualified method calls are resolved by the type checker rather
            // than ResolvePass, so supplement the symbol index from open,
            // unsaved documents using the exact module qualifier.
            if (!qualifier.empty())
                for (const auto& [_, open] : m_documents) {
                    Lexer lexer(open.text, open.path);
                    for (const auto& token : lexer.tokenizeAll()) {
                        if ((token.type != TokenType::LowerIdent &&
                             token.type != TokenType::UpperIdent) ||
                            token.value != word.text)
                            continue;
                        if (moduleQualifierBeforeWord(
                                open.text,
                                static_cast<unsigned int>(token.location.line - 1),
                                static_cast<uint32_t>(token.location.column)) ==
                            qualifier)
                            append(token.location);
                    }
                }
            if (!qualifier.empty()) {
                const auto ownerKey = qualifier + "." + word.text;
                for (const auto& reference : m_indexedCallReferences)
                    if (reference.ownerKey == ownerKey)
                        append(reference.location);
            }
            return result;
        }


        // Imported receiver methods have no source SymbolInfo in the current
        // compilation unit. The analyzer still records the exact selected
        // owner for every call; use it to keep overload families such as
        // Optional.or and Bits.or separate.
        const Document::HoverEntry* selectedCall = nullptr;
        for (const auto& entry : document.selectedCallEntries)
            if (entry.line == params.position.line + 1 &&
                word.byteColumn >= entry.byteColumn &&
                word.byteColumn < entry.byteColumn + entry.byteLength) {
                selectedCall = &entry;
                break;
            }
        if (selectedCall && !selectedCall->documentationKey.empty()) {
            const auto separator = selectedCall->documentationKey.rfind('.');
            if (params.context.includeDeclaration &&
                separator != std::string::npos)
                if (const auto* declaration = m_db.receiverSymbol(
                        selectedCall->documentationKey.substr(0, separator),
                        word.text))
                    append(declaration->definition);
            for (const auto& [_, open] : m_documents)
                for (const auto& entry : open.selectedCallEntries)
                    if (entry.documentationKey ==
                        selectedCall->documentationKey)
                        append(SourceLocation{
                            open.path, static_cast<int>(entry.line),
                            static_cast<int>(entry.byteColumn)});
            for (const auto& reference : m_indexedCallReferences)
                if (reference.ownerKey == selectedCall->documentationKey)
                    append(reference.location);
            return result;
        }

        size_t lineStart = 0;
        for (unsigned int current = 0; current < params.position.line;
             ++current) {
            const auto newline = document.text.find('\n', lineStart);
            if (newline == std::string::npos) break;
            lineStart = newline + 1;
        }
        const auto wordOffset = lineStart + word.byteColumn - 1;
        auto declaration = sourceLocalDefinition(
            document.text, document.path, word.text,
            std::min(document.text.size() - 1,
                     wordOffset + word.text.size()));
        if (!declaration) return result;
        if (params.context.includeDeclaration) append(*declaration);

        Lexer lexer(document.text, document.path);
        for (const auto& token : lexer.tokenizeAll()) {
            if ((token.type != TokenType::LowerIdent &&
                 token.type != TokenType::UpperIdent) ||
                token.value != word.text || token.startOffset < 0)
                continue;
            if (sameLocation(token.location, *declaration)) continue;
            const auto before = token.startOffset == 0
                ? 0 : static_cast<size_t>(token.startOffset - 1);
            if (auto owner = sourceLocalDefinition(
                    document.text, document.path, word.text, before);
                owner && sameLocation(*owner, *declaration))
                append(token.location);
        }
        return result;
    }

    void registerHandlers() {
        m_handler
            .add<::lsp::requests::Initialize>(
                [this](::lsp::InitializeParams&& params) {
                    return initialize(std::move(params));
            })
            .add<::lsp::notifications::Initialized>(
                [](::lsp::InitializedParams&&) {})
            .add<::lsp::requests::Shutdown>([this]() {
                m_cleanShutdown = true;
                return ::lsp::requests::Shutdown::Result{};
            })
            .add<::lsp::notifications::Exit>([this]() { m_running = false; })
            .add<::lsp::notifications::TextDocument_DidOpen>(
                [this](::lsp::DidOpenTextDocumentParams&& params) {
                    auto& document = params.textDocument;
                    update(document.uri, std::move(document.text), document.version);
                })
            .add<::lsp::notifications::TextDocument_DidChange>(
                [this](::lsp::DidChangeTextDocumentParams&& params) {
                    if (params.contentChanges.empty()) return;
                    auto& change = params.contentChanges.back();
                    if (auto* whole = std::get_if<::lsp::TextDocumentContentChangeWholeDocument>(&change))
                        update(params.textDocument.uri, std::move(whole->text),
                               params.textDocument.version);
                })
            .add<::lsp::notifications::TextDocument_DidSave>(
                [this](::lsp::DidSaveTextDocumentParams&& params) {
                    verifyInitialized();
                    const auto found = m_documents.find(params.textDocument.uri.toString());
                    if (found != m_documents.end())
                        publishDiagnostics(params.textDocument.uri, found->second);
                })
            .add<::lsp::notifications::TextDocument_DidClose>(
                [this](::lsp::DidCloseTextDocumentParams&& params) {
                    verifyInitialized();
                    const auto key = params.textDocument.uri.toString();
                    if (auto found = m_documents.find(key); found != m_documents.end()) {
                        m_db.removeFile(found->second.path);
                        m_documents.erase(found);
                        m_referenceIndexReady = false;
                        m_referenceIndexWord.clear();
                    }
                    m_handler.sendNotification<
                        ::lsp::notifications::TextDocument_PublishDiagnostics>({
                        .uri = params.textDocument.uri,
                        .diagnostics = {},
                    });
                })
            .add<::lsp::requests::TextDocument_Completion>(
                [this](::lsp::CompletionParams&& params) {
                    return completions(std::move(params));
                })
            .add<::lsp::requests::TextDocument_Hover>(
                [this](::lsp::HoverParams&& params) {
                    return hover(std::move(params));
                })
            .add<::lsp::requests::TextDocument_Definition>(
                [this](::lsp::DefinitionParams&& params) {
                    return definition(std::move(params));
                })
            .add<::lsp::requests::TextDocument_References>(
                [this](::lsp::ReferenceParams&& params) {
                    return references(std::move(params));
                });
    }

    Iostream m_stream;
    ::lsp::Connection m_connection;
    ::lsp::MessageHandler m_handler;
    semantic::ImportedInterfaces m_interfaces;
    std::unordered_map<std::string, std::vector<std::string>>
        m_standardDocumentation;
    std::unordered_map<std::string, std::vector<std::vector<std::string>>>
        m_standardParameterNames;
    semantic::SemanticDB m_db;
    semantic::SemanticDB m_referenceDb;
    std::unordered_map<std::string, Document> m_documents;
    std::vector<std::string> m_workspaceRoots;
    std::vector<IndexedCallReference> m_indexedCallReferences;
    std::vector<std::string> m_referenceIndexedPaths;
    bool m_referenceIndexReady = false;
    std::string m_referenceIndexWord;
    bool m_running = true;
    bool m_initialized = false;
    bool m_cleanShutdown = false;
};

} // namespace

auto run(std::istream& input, std::ostream& output,
         const std::string& runtimeBeamDir) -> int {
    try {
        return Server(input, output, runtimeBeamDir).run();
    } catch (const std::exception& error) {
        std::cerr << "kex --lsp: " << error.what() << '\n';
        return 1;
    }
}

} // namespace kex::lsp

#include "api_help.hxx"

#include "../ast/ast.hxx"
#include "../ast/convert.hxx"
#include "../ast/type_text.hxx"
#include "../common/prelude_loader.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <variant>

namespace kex::help {

namespace {

// How many matches `kex -s <member>` prints in full before it falls back to
// one line each: `map` is declared on a dozen types, and a dozen doc comments
// bury the one the caller wanted.
constexpr size_t kFullDetailLimit = 4;
// How many near names a failed lookup offers.
constexpr size_t kSuggestionLimit = 12;
// Summaries start in one column, but a single long signature must not push
// that column off the screen for every other member.
constexpr size_t kSignatureColumnLimit = 44;

auto trimmed(std::string_view text) -> std::string {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

auto lowered(std::string_view text) -> std::string {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

auto lastSegment(std::string_view name) -> std::string_view {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

auto splitLines(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

auto withoutTrailingDo(std::string line) -> std::string {
    constexpr std::string_view suffix = " do";
    if (line.size() > suffix.size() &&
        line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0)
        line.resize(line.size() - suffix.size());
    return line;
}

// The stdlib writes inline code the RDoc way, `+None+`. Backticks say the
// same thing to every reader this output has, so prose lines are respelled;
// an indented line is example code, where a `+` is an operator.
auto proseMarkup(const std::string& doc) -> std::string {
    std::string result;
    for (const auto& line : splitLines(doc)) {
        if (!result.empty()) result += "\n";
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            result += line;
            continue;
        }
        size_t at = 0;
        while (at < line.size()) {
            const bool opens =
                line[at] == '+' && at + 1 < line.size() &&
                line[at + 1] != ' ' &&
                (at == 0 || line[at - 1] == ' ' || line[at - 1] == '(');
            auto close = opens ? line.find('+', at + 2) : std::string::npos;
            // The closing mark hugs the code and ends the word: the `+` of
            // `a + b` does neither.
            while (close != std::string::npos &&
                   (line[close - 1] == ' ' ||
                    (close + 1 < line.size() &&
                     std::isalnum(
                         static_cast<unsigned char>(line[close + 1])))))
                close = line.find('+', close + 1);
            const bool closes = close != std::string::npos;
            if (!closes) {
                result += line[at++];
                continue;
            }
            result += "`" + line.substr(at + 1, close - at - 1) + "`";
            at = close + 1;
        }
    }
    return result;
}

// Where the first sentence of `paragraph` ends: at a period followed by a
// space and the start of a new sentence, a capital or a code mark. The period
// of `i.e. the` or `e.g. 1.5` is followed by a lower-case word or a digit, so
// it does not end anything.
auto sentenceEnd(const std::string& paragraph) -> size_t {
    for (auto at = paragraph.find(". "); at != std::string::npos;
         at = paragraph.find(". ", at + 1)) {
        if (at + 2 >= paragraph.size()) break;
        const auto next = static_cast<unsigned char>(paragraph[at + 2]);
        if (std::isupper(next) || next == '+' || next == '`') return at;
    }
    return std::string::npos;
}

// The first sentence of a doc comment: what a listing shows beside a name.
auto summaryOf(const std::string& doc) -> std::string {
    std::string paragraph;
    for (const auto& line : splitLines(doc)) {
        const auto text = trimmed(line);
        if (text.empty() || text[0] == '@') break;
        if (!paragraph.empty()) paragraph += " ";
        paragraph += text;
    }
    const auto end = sentenceEnd(paragraph);
    if (end != std::string::npos) paragraph.resize(end + 1);
    return proseMarkup(paragraph);
}

// The name a `make` target is looked up under. `[X]`, `[Number]` and `[[Y]]`
// are all blocks of methods on a list, and `kex -s List` must find the three.
auto ownerNameOf(const ast::TypeExpr& target) -> std::string {
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>)
            return ast::typeNameText(node);
        else if constexpr (std::is_same_v<T, ast::GenericType>)
            return ast::typeNameText(node.name);
        else if constexpr (std::is_same_v<T, ast::ListType>)
            return "List";
        else if constexpr (std::is_same_v<T, ast::MapType>)
            return "Map";
        else if constexpr (std::is_same_v<T, ast::OptionalType>)
            return "Optional";
        else if constexpr (std::is_same_v<T, ast::TupleType>)
            return "Tuple";
        else if constexpr (std::is_same_v<T, ast::FunctionType>)
            return "Function";
        else
            return ast::typeExprText(target);
    }, target.kind);
}

auto signatureOf(const ast::TypeAnnotation& annotation) -> std::string {
    std::string result = annotation.isFoul ? "foul " : "";
    result += annotation.name;
    result += annotation.implicitFrom ? " ::> "
              : annotation.implicitThis ? " :> " : " : ";
    if (annotation.type) result += ast::typeExprText(*annotation.type);
    return result;
}

// A function typed where it is defined, `let from(items: [A]) -> Set<A>`,
// spelled as the signature line it could have been given instead, so one
// listing reads one way. Falls back to the `let` spelling when a parameter
// has no type to put in that line.
auto signatureOf(const ast::FunctionDef& function, bool isMethod)
    -> std::string {
    std::string result = function.isFoul ? "foul " : "";
    if (function.clauses.empty()) return result + function.name;
    const auto& clause = function.clauses.front();
    const bool typed = std::all_of(
        clause.params.begin(), clause.params.end(),
        [](const ast::Param& param) { return param.type && *param.type; });
    if (!typed || !clause.returnAnnotation || !*clause.returnAnnotation) {
        result += "let " + function.name;
        // No parameters, no parentheses: as it is declared and as it is called.
        if (!clause.params.empty()) result += "(";
        for (size_t i = 0; i < clause.params.size(); ++i) {
            const auto& param = clause.params[i];
            if (i) result += ", ";
            result += param.name.value_or("_");
            if (param.type && *param.type)
                result += ": " + ast::typeExprText(**param.type);
        }
        if (!clause.params.empty()) result += ")";
        if (clause.returnAnnotation && *clause.returnAnnotation)
            result += " -> " + ast::typeExprText(**clause.returnAnnotation);
        return result;
    }
    result += function.name + (isMethod ? " :> " : " : ");
    for (const auto& param : clause.params) {
        const auto& type = **param.type;
        const bool isFunction =
            std::holds_alternative<ast::FunctionType>(type.kind) &&
            !type.parenthesized;
        const auto text = ast::typeExprText(type);
        result += (isFunction ? "(" + text + ")" : text) + " -> ";
    }
    return result + ast::typeExprText(**clause.returnAnnotation);
}

// Walks one parsed source and files what it declares under the owners that a
// caller would name. Private declarations never reach the index: help that
// lists a function the checker then refuses is worse than no help.
class Collector {
public:
    Collector(std::map<std::string, ApiOwner>& owners,
              const std::unordered_map<int, std::string>& docs,
              const std::vector<std::string>& lines)
        : m_owners(owners), m_docs(docs), m_lines(lines) {}

    auto run(const ast::Program& program) -> void {
        Scope top{"", declaredTypes(program.items), {}};
        noteAnnotated(program.items, top.annotated);
        for (const auto& item : program.items)
            std::visit([&](const auto& ptr) { collect(*ptr, top); }, item);
    }

    // Owners this source declared, in order: the names its `using` brings in.
    auto declared() const -> const std::vector<std::string>& {
        return m_declared;
    }

    // Members this source added, as owner and position. A source may add
    // methods to an owner it does not declare (`make Integer` in a units
    // module), and those need this source's `using` though the owner does not.
    auto added() const
        -> const std::vector<std::pair<std::string, size_t>>& {
        return m_added;
    }

private:
    struct Scope {
        std::string module;
        // Types and records this module declares, by their bare names: a
        // `make Path` inside `module FS` extends `FS.Path`.
        std::set<std::string> types;
        // Functions that have a signature line, which then speaks for them.
        std::set<std::string> annotated;
    };

    template <typename Items>
    static auto noteAnnotated(const Items& items, std::set<std::string>& names)
        -> void {
        for (const auto& item : items)
            std::visit([&](const auto& ptr) {
                using T = std::decay_t<decltype(*ptr)>;
                if constexpr (std::is_same_v<T, ast::TypeAnnotation>)
                    names.insert(ptr->name);
                else if constexpr (std::is_same_v<T, ast::VisibilityBlock>)
                    noteAnnotated(ptr->items, names);
            }, item);
    }

    template <typename Items>
    static auto declaredTypes(const Items& items) -> std::set<std::string> {
        std::set<std::string> names;
        for (const auto& item : items)
            std::visit([&](const auto& ptr) {
                using T = std::decay_t<decltype(*ptr)>;
                if constexpr (std::is_same_v<T, ast::TypeDef> ||
                              std::is_same_v<T, ast::RecordDef> ||
                              std::is_same_v<T, ast::TraitDef>)
                    names.insert(ptr->name);
            }, item);
        return names;
    }

    auto qualified(const std::string& name, const Scope& scope) const
        -> std::string {
        if (scope.module.empty() || name.find('.') != std::string::npos)
            return name;
        return scope.module + "." + name;
    }

    auto docAt(int line) const -> std::string {
        const auto found = m_docs.find(line);
        return found == m_docs.end() ? std::string{} : found->second;
    }

    auto lineAt(int line) const -> std::string {
        if (line < 1 || static_cast<size_t>(line) > m_lines.size()) return {};
        return m_lines[static_cast<size_t>(line) - 1];
    }

    // A declaration as its author wrote it. A block form runs to the `end`
    // that closes it, without the comments and blank lines in between, so a
    // record shows its fields and a sum type every variant.
    auto declarationAt(int line, bool withBody) const -> std::string {
        const auto first = lineAt(line);
        auto result = trimmed(first);
        const auto indent = first.find_first_not_of(" \t");
        const bool isBlock = result.size() > 3 &&
                             result.compare(result.size() - 3, 3, " do") == 0;
        if (!isBlock) {
            for (int next = line + 1;; ++next) {
                const auto text = trimmed(lineAt(next));
                if (text.empty() || text[0] != '|') break;
                result += "\n  " + text;
            }
            return result;
        }
        if (!withBody) return withoutTrailingDo(result);
        for (int next = line + 1;
             static_cast<size_t>(next) <= m_lines.size(); ++next) {
            const auto raw = lineAt(next);
            const auto text = trimmed(raw);
            if (text.empty() || text[0] == '#') continue;
            if (text == "end" && raw.find_first_not_of(" \t") == indent) {
                result += "\nend";
                break;
            }
            result += "\n  " + text;
        }
        return result;
    }

    auto owner(const std::string& name, OwnerKind kind, bool declares)
        -> ApiOwner& {
        const auto [found, inserted] = m_owners.try_emplace(name);
        auto& result = found->second;
        if (inserted) {
            result.name = name;
            result.kind = kind;
        } else if (declares) {
            // A `make` that came first only guessed at the kind.
            result.kind = kind;
        }
        if (declares && std::find(m_declared.begin(), m_declared.end(),
                                  name) == m_declared.end())
            m_declared.push_back(name);
        return result;
    }

    auto addMember(const ast::TypeAnnotation& annotation, ApiOwner& into,
                   const std::string& group) -> void {
        m_added.emplace_back(into.name, into.members.size());
        into.members.push_back(ApiMember{
            annotation.name, signatureOf(annotation), group,
            docAt(annotation.location.line), {}});
    }

    auto addMember(const ast::FunctionDef& function, ApiOwner& into,
                   const std::string& group, bool isMethod,
                   const std::set<std::string>& annotated) -> void {
        if (annotated.count(function.name)) return;
        auto signature = signatureOf(function, isMethod);
        // Clauses of one function, `let f(0)` then `let f(n)`, are one entry.
        for (const auto& member : into.members)
            if (member.group == group && member.signature == signature)
                return;
        m_added.emplace_back(into.name, into.members.size());
        into.members.push_back(ApiMember{function.name, std::move(signature),
                                         group, docAt(function.location.line),
                                         {}});
    }

    // The body of a `make` or a `trait`: every member is a method of `into`.
    template <typename Items>
    auto addMethods(const Items& items, ApiOwner& into,
                    const std::string& group,
                    const std::set<std::string>& annotated) -> void {
        for (const auto& item : items)
            std::visit([&](const auto& ptr) {
                using T = std::decay_t<decltype(*ptr)>;
                if constexpr (std::is_same_v<T, ast::TypeAnnotation>) {
                    addMember(*ptr, into, group);
                } else if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                    addMember(*ptr, into, group, true, annotated);
                } else if constexpr (std::is_same_v<T,
                                                    ast::VisibilityBlock>) {
                    if (ptr->isPublic)
                        addMethods(ptr->items, into, group, annotated);
                }
            }, item);
    }

    auto collect(const ast::ModuleDef& module, const Scope& outer) -> void {
        Scope scope{qualified(module.name, outer),
                    declaredTypes(module.body), {}};
        noteAnnotated(module.body, scope.annotated);
        auto& into =
            owner(scope.module, OwnerKind::Module,
                  !m_owners.count(scope.module) ||
                      m_owners[scope.module].declaration.empty());
        if (into.declaration.empty()) {
            // A module that shares its name with a type holds that type's
            // constructors; the type's own declaration is the one to show.
            into.kind = OwnerKind::Module;
            into.declaration =
                std::string(module.isCapability ? "capability " : "module ") +
                scope.module;
            into.doc = docAt(module.location.line);
        }
        for (const auto& item : module.body)
            std::visit([&](const auto& ptr) { collect(*ptr, scope); }, item);
    }

    auto collect(const ast::VisibilityBlock& block, const Scope& scope)
        -> void {
        if (!block.isPublic) return;
        for (const auto& item : block.items)
            std::visit([&](const auto& ptr) { collect(*ptr, scope); }, item);
    }

    auto collect(const ast::TypeAnnotation& annotation, const Scope& scope)
        -> void {
        // A source with no `module` of its own declares into the top level.
        addMember(annotation, owner(scope.module, OwnerKind::Module, false),
                  "");
    }

    auto collect(const ast::FunctionDef& function, const Scope& scope)
        -> void {
        addMember(function, owner(scope.module, OwnerKind::Module, false), "",
                  false, scope.annotated);
    }

    auto collect(const ast::TypeDef& type, const Scope& scope) -> void {
        auto& into = owner(qualified(type.name, scope), OwnerKind::Type, true);
        into.declaration = declarationAt(type.location.line, false);
        into.doc = docAt(type.location.line);
    }

    auto collect(const ast::RecordDef& record, const Scope& scope) -> void {
        auto& into =
            owner(qualified(record.name, scope), OwnerKind::Record, true);
        into.declaration = declarationAt(record.location.line, true);
        into.doc = docAt(record.location.line);
    }

    auto collect(const ast::TraitDef& trait, const Scope& scope) -> void {
        auto& into =
            owner(qualified(trait.name, scope), OwnerKind::Trait, true);
        into.declaration = declarationAt(trait.location.line, false);
        into.doc = docAt(trait.location.line);
        std::set<std::string> annotated;
        noteAnnotated(trait.body, annotated);
        addMethods(trait.body, into, "", annotated);
    }

    auto collect(const ast::MakeDef& make, const Scope& scope) -> void {
        if (!make.target) return;
        auto name = ownerNameOf(*make.target);
        if (scope.types.count(name)) name = qualified(name, scope);
        auto& into = owner(name, OwnerKind::Type, false);
        for (const auto& implemented : make.implements)
            if (std::find(into.traits.begin(), into.traits.end(),
                          implemented) == into.traits.end())
                into.traits.push_back(implemented);
        const auto group = "make " + ast::typeExprText(*make.target);
        std::set<std::string> annotated;
        noteAnnotated(make.body, annotated);
        addMethods(make.body, into, group, annotated);
    }

    // Everything else a source may hold — function bodies, `using`, `main`,
    // compile-time blocks — declares nothing a caller looks up by name.
    template <typename Other>
    auto collect(const Other&, const Scope&) -> void {}

    std::map<std::string, ApiOwner>& m_owners;
    const std::unordered_map<int, std::string>& m_docs;
    const std::vector<std::string>& m_lines;
    std::vector<std::string> m_declared;
    std::vector<std::pair<std::string, size_t>> m_added;
};

auto kindWord(OwnerKind kind) -> std::string_view {
    switch (kind) {
    case OwnerKind::Module: return "module";
    case OwnerKind::Type: return "type";
    case OwnerKind::Record: return "record";
    case OwnerKind::Trait: return "trait";
    }
    return "type";
}

// The index runs to hundreds of names in alphabetical order, which puts
// `Int8` and `Kex.AST` ahead of what a first program needs. These are named
// above it.
constexpr std::string_view kStartingPoints =
    "List, Map, String, Integer, Float, Optional, Result, IO, Math";

// The top level has no name to ask for, so it is listed under one.
constexpr std::string_view kTopLevelName = "(top level)";
constexpr std::string_view kTopLevelSummary =
    "Functions called by their bare names, from anywhere.";

auto displayName(const ApiOwner& owner) -> std::string {
    return owner.name.empty() ? std::string(kTopLevelName) : owner.name;
}

auto memberMatches(const ApiMember& member, std::string_view name) -> bool {
    if (member.name == name) return true;
    // A shell eats the `?` of `kex -s List.empty?` unless it is quoted, so
    // the bare word finds the predicate too.
    return member.name.size() == name.size() + 1 &&
           member.name.compare(0, name.size(), name) == 0 &&
           (member.name.back() == '?' || member.name.back() == '!');
}

auto printMember(const ApiOwner& owner, const ApiMember& member,
                 std::ostream& out) -> void {
    out << displayName(owner) << "." << member.name;
    if (!member.group.empty()) out << "  (" << member.group << ")";
    out << "\n  " << member.signature << "\n";
    const auto& needed = member.requiresUsing.empty() ? owner.requiresUsing
                                                      : member.requiresUsing;
    if (!needed.empty()) out << "  needs `using " << needed << "`\n";
    if (!member.doc.empty()) out << "\n" << proseMarkup(member.doc) << "\n";
}

auto printMemberLine(const ApiMember& member, size_t column,
                     std::ostream& out) -> void {
    out << "  " << member.signature;
    auto summary = summaryOf(member.doc);
    if (!member.requiresUsing.empty())
        summary = "[using " + member.requiresUsing + "]" +
                  (summary.empty() ? "" : " " + summary);
    if (!summary.empty()) {
        if (member.signature.size() < column)
            out << std::string(column - member.signature.size(), ' ');
        out << "  " << summary;
    }
    out << "\n";
}

} // namespace

auto ApiIndex::addSource(const std::string& path, std::string source,
                         const std::string& usingName) -> void {
    const auto lines = splitLines(source);
    Lexer lexer(source, path);
    auto tokens = lexer.tokenizeAll();
    const auto docs = ast::extractDocComments(tokens, source);
    Parser parser(std::move(tokens), path);
    const auto program = parser.parseProgram();

    Collector collector(m_owners, docs, lines);
    collector.run(program);
    if (usingName.empty()) return;

    // The path spells the module in lower case; the source spells it as a
    // program must.
    auto spelled = usingName;
    for (const auto& name : collector.declared())
        if (lowered(name) == lowered(usingName)) spelled = name;
    for (const auto& name : collector.declared())
        m_owners[name].requiresUsing = spelled;
    for (const auto& [name, position] : collector.added()) {
        auto& owner = m_owners[name];
        if (owner.requiresUsing != spelled)
            owner.members[position].requiresUsing = spelled;
    }
}

auto ApiIndex::fromStandardLibrary(std::ostream& warnings) -> ApiIndex {
    ApiIndex index;
    for (const auto& root : standardLibraryRootCandidates()) {
        const auto manifest = preludeManifestPath(root);
        if (!manifest) continue;
        const auto preludeFiles = preludeFilesFromRoot(root);
        const std::set<std::string> prelude(preludeFiles.begin(),
                                            preludeFiles.end());

        std::vector<std::filesystem::path> sources;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root))
            if (entry.is_regular_file() &&
                entry.path().extension() == ".kex" &&
                entry.path() != *manifest)
                sources.push_back(entry.path());
        std::sort(sources.begin(), sources.end());

        for (const auto& path : sources) {
            std::string usingName;
            if (!prelude.count(path.string())) {
                auto relative = path.lexically_relative(root);
                relative.replace_extension();
                for (const auto& segment : relative) {
                    if (!usingName.empty()) usingName += ".";
                    usingName += segment.string();
                }
            }
            try {
                std::ifstream input(path);
                std::ostringstream text;
                text << input.rdbuf();
                index.addSource(path.string(), text.str(), usingName);
            } catch (const std::exception& error) {
                warnings << "warning: " << path.string()
                         << ": skipped: " << error.what() << "\n";
            }
        }
        break;
    }
    return index;
}

auto ApiIndex::findOwners(std::string_view name) const
    -> std::vector<const ApiOwner*> {
    if (name.empty()) return {};
    if (const auto exact = m_owners.find(
            name == kTopLevelName ? std::string{} : std::string(name));
        exact != m_owners.end())
        return {&exact->second};
    const auto wanted = lowered(name);
    std::vector<const ApiOwner*> sameSpelling;
    std::vector<const ApiOwner*> sameLastSegment;
    for (const auto& [ownerName, owner] : m_owners) {
        if (ownerName.empty()) continue;
        const auto candidate = lowered(ownerName);
        if (candidate == wanted) sameSpelling.push_back(&owner);
        else if (lastSegment(candidate) == wanted)
            sameLastSegment.push_back(&owner);
    }
    return sameSpelling.empty() ? sameLastSegment : sameSpelling;
}

auto ApiIndex::traitOwner(const std::string& name) const -> const ApiOwner* {
    for (const auto* owner : findOwners(name))
        if (owner->kind == OwnerKind::Trait) return owner;
    return nullptr;
}

auto ApiIndex::printOverview(std::ostream& out) const -> void {
    out << "The Kex standard library.\n"
           "  kex -s <Name>           everything a type or module offers\n"
           "  kex -s <Name>.<member>  one function in full, with examples\n"
           "  kex -s <member>         every type that has it\n"
           "Most programs are written with: "
        << kStartingPoints << ".\n";

    size_t column = 0;
    for (const auto& [name, owner] : m_owners)
        column = std::max(column, displayName(owner).size());

    const auto section = [&](std::string_view title, bool wantsUsing) {
        out << "\n" << title << "\n";
        for (const auto& [name, owner] : m_owners) {
            if (owner.requiresUsing.empty() == wantsUsing) continue;
            if (owner.members.empty() && owner.doc.empty()) continue;
            const auto shown = displayName(owner);
            out << "  " << shown << std::string(column - shown.size(), ' ')
                << "  " << kindWord(owner.kind);
            const auto summary = name.empty() ? std::string(kTopLevelSummary)
                                              : summaryOf(owner.doc);
            if (!summary.empty())
                out << std::string(6 - kindWord(owner.kind).size(), ' ')
                    << "  " << summary;
            out << "\n";
        }
    };
    section("Always available (the prelude):", false);
    section("After a `using` (kex -s <Name> says which):", true);
}

auto ApiIndex::printOwner(const ApiOwner& owner, std::ostream& out) const
    -> void {
    if (owner.name.empty())
        out << kTopLevelSummary << "\n";
    else
        out << (owner.declaration.empty()
                    ? std::string(kindWord(owner.kind)) + " " + owner.name
                    : owner.declaration)
            << "\n";
    if (!owner.requiresUsing.empty())
        out << "  needs `using " << owner.requiresUsing << "`\n";
    if (!owner.doc.empty()) out << "\n" << proseMarkup(owner.doc) << "\n";

    size_t column = 0;
    for (const auto& member : owner.members)
        if (member.signature.size() <= kSignatureColumnLimit)
            column = std::max(column, member.signature.size());

    std::vector<std::string> groups;
    for (const auto& member : owner.members)
        if (std::find(groups.begin(), groups.end(), member.group) ==
            groups.end())
            groups.push_back(member.group);
    for (const auto& group : groups) {
        out << "\n";
        if (!group.empty()) out << group << "\n";
        for (const auto& member : owner.members)
            if (member.group == group) printMemberLine(member, column, out);
    }

    // What the type's traits add is callable on it exactly like its own
    // methods, and is where `map` and `filter` live.
    for (const auto& traitName : owner.traits) {
        const auto* trait = traitOwner(traitName);
        if (!trait) continue;
        std::string names;
        std::set<std::string> seen;
        for (const auto& member : trait->members) {
            const bool own = std::any_of(
                owner.members.begin(), owner.members.end(),
                [&](const ApiMember& mine) {
                    return mine.name == member.name;
                });
            if (own || !seen.insert(member.name).second) continue;
            if (!names.empty()) names += ", ";
            names += member.name;
        }
        if (names.empty()) continue;
        out << "\nfrom " << trait->name << " (kex -s " << trait->name
            << "): " << names << "\n";
    }

    std::string nested;
    for (const auto& [name, other] : m_owners) {
        if (owner.name.empty() || name.size() <= owner.name.size() ||
            name.compare(0, owner.name.size() + 1, owner.name + ".") != 0)
            continue;
        if (!nested.empty()) nested += ", ";
        nested += name;
    }
    if (!nested.empty()) out << "\nsee also: " << nested << "\n";
}

auto ApiIndex::printTopic(std::string_view query, std::ostream& out,
                          std::ostream& err) const -> bool {
    if (const auto owners = findOwners(query); !owners.empty()) {
        for (size_t i = 0; i < owners.size(); ++i) {
            if (i) out << "\n";
            printOwner(*owners[i], out);
        }
        return true;
    }

    std::vector<std::pair<const ApiOwner*, const ApiMember*>> matches;
    const auto dot = query.rfind('.');
    if (dot != std::string_view::npos) {
        const auto memberName = query.substr(dot + 1);
        for (const auto* owner : findOwners(query.substr(0, dot))) {
            for (const auto& member : owner->members)
                if (memberMatches(member, memberName))
                    matches.emplace_back(owner, &member);
            if (!matches.empty()) continue;
            for (const auto& traitName : owner->traits)
                if (const auto* trait = traitOwner(traitName))
                    for (const auto& member : trait->members)
                        if (memberMatches(member, memberName))
                            matches.emplace_back(trait, &member);
        }
    } else {
        for (const auto& [name, owner] : m_owners)
            for (const auto& member : owner.members)
                if (memberMatches(member, query))
                    matches.emplace_back(&owner, &member);
    }

    if (matches.size() > kFullDetailLimit) {
        for (const auto& [owner, member] : matches) {
            out << displayName(*owner) << "." << member->name << "\n";
            printMemberLine(*member, 0, out);
        }
        out << "\n`kex -s <Name>." << matches.front().second->name
            << "` shows one of these in full.\n";
        return true;
    }
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) out << "\n";
        printMember(*matches[i].first, *matches[i].second, out);
    }
    if (!matches.empty()) return true;

    err << "kex: nothing in the standard library is called `" << query
        << "`\n";
    const auto wanted = lowered(lastSegment(query));
    std::vector<std::string> near;
    for (const auto& [name, owner] : m_owners) {
        if (!name.empty() && lowered(name).find(wanted) != std::string::npos)
            near.push_back(name);
        for (const auto& member : owner.members)
            if (lowered(member.name).find(wanted) != std::string::npos)
                near.push_back(displayName(owner) + "." + member.name);
    }
    near.erase(std::unique(near.begin(), near.end()), near.end());
    if (near.size() > kSuggestionLimit) near.resize(kSuggestionLimit);
    if (!near.empty()) {
        err << "close:";
        for (const auto& name : near) err << " " << name;
        err << "\n";
    }
    err << "`kex -s` lists every type and module.\n";
    return false;
}

} // namespace kex::help

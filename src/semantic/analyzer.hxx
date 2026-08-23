#pragma once

#include "../ast/ast.hxx"
#include "symbol.hxx"
#include "typechecker.hxx"
#include <optional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

struct DiagnosticNote {
    SourceLocation location;
    std::string message;
};

struct Diagnostic {
    enum class Level { Error, Warning };
    Level level;
    SourceLocation location;
    std::string message;
    // Exclusive end position for ranged diagnostics. Point diagnostics leave
    // this empty, preserving the existing line/column behavior.
    std::optional<SourceLocation> endLocation = std::nullopt;
    std::vector<DiagnosticNote> notes;
};

class Analyzer {
public:
    explicit Analyzer(const ImportedInterfaces* importedInterfaces = nullptr)
        : m_checker(importedInterfaces), m_importedInterfaces(importedInterfaces) {}

    auto analyze(const ast::Program& program) -> bool;
    auto diagnostics() const -> const std::vector<Diagnostic>&;

    // Query the inferred type of an expression node after analyze() has run.
    auto typeOf(const ast::Expr* expr) const -> TypePtr;
    // Presentation-widened `typeOf` — see TypeChecker::displayTypeOf.
    auto displayTypeOf(const ast::Expr* expr) const -> TypePtr;
    auto typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>&;
    auto assignmentTargetTypeOf(const ast::Expr* expr) const -> TypePtr;
    // Capabilities `name` needs, each mapped to the callee that carried the
    // requirement in ("" when the function reaches it directly). Nothing is
    // annotated, so this is the only place a reader can learn what a function
    // needs — tooling displays it (kexhq/kex#143).
    auto requiredCapabilities(const std::string& name) const
        -> const std::map<std::string, std::string>* {
        auto found = m_requiredCapabilities.find(name);
        return found == m_requiredCapabilities.end() ? nullptr : &found->second;
    }
    // The chain that explains one requirement, innermost first:
    //   {"deep", "stamp"}, {"stamp", ""}   for `deep requires Clock via stamp`.
    auto capabilityChain(const std::string& name,
                         const std::string& capability) const
        -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> chain;
        std::string at = name;
        std::set<std::string> seen;   // a cyclic call graph must still finish
        while (!at.empty() && seen.insert(at).second) {
            auto found = m_requiredCapabilities.find(at);
            if (found == m_requiredCapabilities.end()) break;
            auto entry = found->second.find(capability);
            if (entry == found->second.end()) break;
            chain.emplace_back(at, entry->second);
            at = entry->second;
        }
        return chain;
    }
    auto isTrait(const std::string& name) const -> bool {
        return m_checker.isTrait(name);
    }
    auto traitRequires(const std::string& trait,
                       const std::string& method) const -> bool {
        return m_checker.traitRequires(trait, method);
    }
    auto traitNeedsDictionary(const std::string& trait) const -> bool {
        return m_checker.traitNeedsDictionary(trait);
    }
    auto functionSignatures(const ast::FunctionDef* function) const
        -> const std::vector<Signature>*;
    auto displaySignature(const std::string& name,
                          const Signature& signature) const -> std::string {
        return m_checker.displaySignature(name, signature);
    }
    auto resolvedCalls() const
        -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>&;
    auto selectedCallSignatures() const
        -> const std::unordered_map<const ast::Expr*, Signature>& {
        return m_checker.selectedCallSignatures();
    }
    auto patternBindings() const
        -> const std::vector<TypeChecker::PatternBinding>& {
        return m_checker.patternBindings();
    }
    auto staticTypeOfCalls() const
        -> const std::unordered_map<const ast::MethodCall*, StaticTypeAnswer>& {
        return m_checker.staticTypeOfCalls();
    }
    auto referencedModules() const
        -> const std::unordered_set<std::string>& {
        return m_checker.referencedModules();
    }

private:
    // Top-level declarations
    auto analyzeTopLevel(const ast::TopLevelItem& item) -> void;
    auto analyzeModule(const ast::ModuleDef& mod) -> void;
    auto analyzeTypeDef(const ast::TypeDef& def) -> void;
    auto analyzeRecordDef(const ast::RecordDef& def) -> void;
    auto analyzeMakeDef(const ast::MakeDef& def) -> void;
    auto analyzeVisibilityBlock(const ast::VisibilityBlock& block) -> void;
    auto analyzeFunctionDef(const ast::FunctionDef& def) -> void;
    auto analyzeMainBlock(const ast::MainBlock& block) -> void;

    // Expressions
    auto analyzeExpr(const ast::Expr& expr) -> void;
    auto analyzeBody(const std::vector<ast::ExprPtr>& body) -> void;
    auto analyzeRescue(const ast::RescueBlock& rescue, SourceLocation loc) -> void;

    // Defines every variable a pattern introduces (VarPattern, shorthand
    // record fields, nested constructor/list/tuple args) as a Symbol in
    // the current scope — mirrors TypeChecker::bindPatternVars, just
    // populating SymbolTable instead of TypeEnv.
    auto bindPatternVars(const ast::Pattern& pat, SourceLocation loc) -> void;

    // Records which member of which module is foul, for the modules declared
    // in THIS compilation unit. Runs before Phase 1 so a call can be checked
    // against a module declared later in the file. Imported modules answer the
    // same question through `isExportFoul`.
    auto collectModuleMemberEffects(const ast::Program& program) -> void;
    // Whether `module.member` is a foul function, consulting local modules
    // first and imported interfaces second. This is the ONLY purity question
    // asked about a qualified call — there is no module-level foulness.
    auto isQualifiedCallFoul(const std::string& module,
                             const std::string& member) const -> bool;

    // Transitive effect computation — runs after Phase 1, before Phase 2.
    auto computeTransitiveEffects(const ast::Program& program) -> void;
    // Post-typechecking enrichment: feed resolved call isFoul into the
    // transitive set and re-check guards.
    auto enrichEffectsFromResolvedCalls(const ast::Program& program) -> void;
    // Qualified names of every module declared `capability` (kexhq/kex#143).
    std::unordered_set<std::string> m_capabilityModules;
    // Which capabilities each function needs, and why: the value maps a
    // capability to the callee that introduced the requirement, or "" when
    // the function reaches the capability itself. One witness per pair is
    // enough to print the chain that explains an otherwise invisible
    // requirement — nothing is annotated, so the explanation is the only
    // place a reader can learn it.
    std::map<std::string, std::map<std::string, std::string>>
        m_requiredCapabilities;
    // Set only while processFn walks one function's body.
    std::map<std::string, std::string>* m_capabilityTarget = nullptr;

    // Loop control
    auto checkLoopControl(SourceLocation loc, const std::string& keyword) -> void;

    // Helpers
    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto warning(SourceLocation loc, const std::string& msg) -> void;

    SymbolTable m_symbols;
    std::vector<Diagnostic> m_diagnostics;
    TypeChecker m_checker;
    const ImportedInterfaces* m_importedInterfaces = nullptr;
    bool m_inFoulContext = false;
    bool m_inMakeBlock = false;

    // break/next bind to the nearest enclosing Loop marker, but a Closure
    // marker in between makes them illegal — they don't cross into a
    // do-block passed to another function. `match`/`receive` clauses don't
    // push a marker, so break/next see through them to the loop.
    enum class LoopScope { Loop, Closure };
    std::vector<LoopScope> m_loopScopes;

    // Transitive effect state — populated by computeTransitiveEffects,
    // enriched by enrichEffectsFromResolvedCalls after type checking.
    std::unordered_map<std::string, std::set<std::string>> m_callGraph;
    std::unordered_set<std::string> m_transitivelyFoul;

    // Foul members of modules declared in this unit, keyed by the module's
    // bare name — nested modules are addressed by their own name (`File`, not
    // `FS.File`), matching how imported interfaces register them.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_localModuleFoulMembers;
};

} // namespace kex::semantic

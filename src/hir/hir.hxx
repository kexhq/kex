#pragma once
// The checked program — Stage 2's HIR. Built once from a parsed program and
// the type checker's analysis of it, it answers the two questions every later
// pass asks: what each call is (`targetOf`) and what type each expression has
// (`typeOf`). Lowering, `--dump-hir` and `--verify-hir` read it; the walker
// and the language server are to follow. It indexes the AST rather than
// copying it, so building it is one pass over the checker's tables.
#include "call_targets.hxx"
#include "../ast/ast.hxx"
#include "../semantic/decided_call.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "../semantic/traits.hxx"
#include "../semantic/type_test.hxx"
#include "../semantic/types.hxx"

#include <functional>
#include <memory>

#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {
class Analyzer;
struct Signature;
}

namespace kex::hir {

// What a call is, as semantic analysis decided it.
struct Target {
    TargetKind kind = TargetKind::Undecided;
    // An imported target's backend module ("" for this unit's own) and
    // function, and the arity it is called at.
    std::string module;
    std::string function;
    int arity = 0;
    // The decided owner: the record a field read reads, the ADT a constructor
    // builds, the type whose operator applies, a builtin's subject type, ...
    std::string owner;
    // A `serving` slot called with no reply to wait for: a cast.
    bool servingCast = false;
    // A built-in operator's operands (BinaryOp only).
    semantic::BuiltinOperands operands = semantic::BuiltinOperands::Checked;
    // An Operator that negates the implementation it names (`!=` as `==`).
    bool negated = false;
};

// The checker's answers a Program reads besides its call targets: borrowed
// from an analysis, or owned by a Program loaded from a cache (persist.hxx).
struct Answers {
    std::unordered_map<const ast::Expr*, semantic::TypePtr> types;
    std::unordered_map<const ast::MethodCall*, semantic::StaticTypeAnswer> typeOfAnswers;
    std::unordered_map<const ast::Expr*, semantic::Signature> selected;
    semantic::ShownThrough shownThroughDistinct;
    // The trait defaults each `make` block inherits (inheritedDefaultsOf).
    std::unordered_map<const ast::MakeDef*, std::vector<semantic::InheritedDefault>> inheritedDefaults;
    // The list patterns that take a String apart (destructuresString).
    std::unordered_set<const ast::Pattern*> stringPatterns;
};

class Program {
public:
    Program(const ast::Program& ast, const semantic::Analyzer& analysis);
    // A program whose answers were loaded (persist.hxx), not analysed here:
    // `targets` keyed by expression, `answers` owned.
    Program(const ast::Program& ast,
            const std::unordered_map<const ast::Expr*, Target>& targets,
            std::unique_ptr<Answers> answers);


    // What `call` is; a Target of kind Undecided when nothing decided it.
    auto targetOf(const ast::Expr& call) const -> const Target&;
    auto targetOf(const ast::MethodCall& call) const -> const Target&;
    auto targetOf(const ast::FunctionCall& call) const -> const Target&;
    auto targetOf(const ast::BinaryOp& op) const -> const Target&;
    // The nominal type a method call was decided on, without type arguments:
    // an OwnMethod's owner, else the receiver of the signature the checker
    // selected (`Process.Shared` for `site.get`) when the call's receiver is
    // of that type; "" otherwise.
    auto receiverTypeOf(const ast::Expr& call) const -> std::string;
    // The static type the checker gave `expr`, or null.
    auto typeOf(const ast::Expr& expr) const -> semantic::TypePtr;
    // A type named as a value (`String`, the checker's `Type<String>`) as the
    // `Type` record both backends build for it, or nullopt.
    auto typeValueOf(const ast::Expr& expr) const -> std::optional<semantic::StructuredType>;
    // The checker's compile-time answer to `Type.of(x)` at `call`, or null.
    auto typeOfAnswer(const ast::MethodCall& call) const
        -> const semantic::StaticTypeAnswer*;
    // The signature the checker selected for `call`, or null.
    auto selectedSignatureOf(const ast::Expr& call) const -> const semantic::Signature*;
    // The same, for the call node inside the expression (what lowering holds).
    auto selectedSignatureOf(const ast::FunctionCall& call) const -> const semantic::Signature*;
    auto selectedSignatureOf(const ast::MethodCall& call) const -> const semantic::Signature*;
    // The distinct type whose `showValue` shows `call`'s argument at parameter
    // `index` (TypeChecker::shownThroughDistinct), or null.
    auto shownThroughOf(const ast::Expr& call, std::size_t index) const -> const std::string*;
    // Every such argument of `call`, or null.
    auto shownThroughOf(const ast::Expr& call) const
        -> const std::vector<std::pair<std::size_t, std::string>>*;

    // The imported function the checker resolved `call` to, with its placed
    // arguments, defaults and trait dictionaries; null for any other call,
    // and for a loaded program.
    auto resolvedTargetOf(const ast::MethodCall& call) const
        -> const semantic::ResolvedCallTarget*;
    // The type whose implementation answers a trait dictionary for `value`
    // (TypeChecker::dictionaryTypeOf); "" for a loaded program.
    auto dictionaryTypeOf(const ast::Expr& value) const -> std::string;
    // The trait a parameter is declared as, which it takes a dictionary for;
    // "" for any other parameter, and for a loaded program.
    auto parameterTraitOf(const ast::Param& param) const -> std::string;
    // The trait a type spelling written in module `scope` names, as the
    // checker resolved it there; "" for any other spelling, and for a loaded
    // program.
    auto traitWrittenAs(const std::string& scope, const std::string& written) const
        -> std::string;
    // The declaration a `make` method is for (`Culture.Culture` for a method
    // of `make Culture`), and the record a named record pattern names, as the
    // checker resolved them; "" otherwise, and for a loaded program.
    // The function a bare call names (`Units.SI.displaySymbol`, `fetch`), as
    // the checker resolved the name in its scope, whichever clause runs; ""
    // when it resolved none, and for a loaded program.
    auto calledFunctionOf(const ast::Expr& call) const -> std::string;
    // The module constant a bare name (`LIMIT`) names, as resolved in its
    // scope; "" otherwise, and for a loaded program.
    auto referencedConstantOf(const ast::Expr& name) const -> std::string;
    auto makeTargetOf(const ast::FunctionDef& method) const -> std::string;
    // The record `expr` builds (a construction), as the checker typed it; "".
    auto recordOf(const ast::Expr& expr) const -> std::string;
    auto recordOfPattern(const ast::RecordPattern& pattern) const -> std::string;
    // The record a written type annotation names, as the checker resolved it
    // where it was written; "" for any other type, and for a loaded program.
    auto recordAnnotatedBy(const ast::TypeExpr& type) const -> std::string;
    // The checker's answers about written annotations, for what a backend
    // builds from them (run-time reflection and dispatch guards): a type as
    // declared with the program's aliases followed, the same for a type
    // value, an annotation as a dispatcher tests it, and what a type spelled
    // `written` in module `scope` says about a value's form. Empty for a
    // loaded program.
    auto declaredTypeOf(const ast::TypeExpr& type) const -> semantic::StructuredType;
    auto expandDeclared(semantic::StructuredType type) const -> semantic::StructuredType;
    auto annotationSpelling(const ast::TypeExpr& type) const -> std::string;
    auto typeTestOf(const std::string& scope, const std::string& written) const
        -> semantic::TypeTest;
    // The trait a method's `make` block is for (`make Shape do`), whose
    // dictionary the method takes for its receiver; "" for any other method,
    // and for a loaded program.
    auto makeTargetTraitOf(const ast::FunctionDef& method) const -> std::string;
    // Whether `pattern`, a parameter, is a bare type name taking that type as
    // a value (`let to(String)`); false for a loaded program.
    auto isTypeSelector(const ast::Pattern& pattern) const -> bool;
    // The traits a `make` block implements, each as the checker resolved its
    // `implement:` name, in written order; null when it names none.
    auto traitsOf(const ast::MakeDef& make) const -> const std::vector<std::string>*;
    // The trait defaults a `make` block inherits for its target: those of its
    // traits the type does not define itself in any of its blocks. Null for a
    // loaded program.
    auto inheritedDefaultsOf(const ast::MakeDef& make) const
        -> const std::vector<semantic::InheritedDefault>*;
    // Whether `pattern`, a list pattern, takes a String apart into its
    // characters (TypeChecker::stringPatterns).
    auto destructuresString(const ast::Pattern& pattern) const -> bool;

    // Every node it has an answer for, each key of its tables once or more.
    auto forEachAnsweredNode(const std::function<void(const void*)>& visit) const -> void;

    // Every call site, in source order, with its target.
    auto callSites() const -> const std::vector<CallSite>& { return m_sites; }

private:
    auto collectSites() -> void;

    const ast::Program* m_ast;
    const semantic::Analyzer* m_analysis = nullptr;
    // Borrowed from the analysis, or `m_owned`'s.
    const decltype(Answers::types)* m_types = nullptr;
    const decltype(Answers::typeOfAnswers)* m_typeOfAnswers = nullptr;
    const decltype(Answers::selected)* m_selected = nullptr;
    const decltype(Answers::inheritedDefaults)* m_inheritedDefaults = nullptr;
    const decltype(Answers::stringPatterns)* m_stringPatterns = nullptr;
    const decltype(Answers::shownThroughDistinct)* m_shownThrough = nullptr;
    std::unique_ptr<Answers> m_owned;
    std::vector<CallSite> m_sites;
    // Keyed by the call node (the Expr, and the MethodCall / BinaryOp inside
    // it), so a pass holding either finds the same target.
    std::unordered_map<const void*, Target> m_targets;
    // The selected signatures by call node.
    std::unordered_map<const void*, const semantic::Signature*> m_selectedByNode;
};

// Several checked programs read as one, newest first: a REPL session's
// inputs, each checked on its own, whose definitions later inputs call.
class ProgramSet {
public:
    auto clear() -> void {
        m_programs.clear();
        m_owners.clear();
    }
    // `program` answers first from now on.
    auto push(const Program* program) -> void {
        if (!program) return;
        m_programs.push_back(program);
        program->forEachAnsweredNode([&](const void* node) { m_owners[node] = program; });
    }
    auto empty() const -> bool { return m_programs.empty(); }
    // Read after every pushed one: the prelude's, which outlives them all.
    auto setBase(const Program* program) -> void { m_base = program; }

    auto targetOf(const ast::MethodCall& call) const -> const Target&;
    auto targetOf(const ast::FunctionCall& call) const -> const Target&;
    auto targetOf(const ast::BinaryOp& op) const -> const Target&;
    auto typeOf(const ast::Expr& expr) const -> semantic::TypePtr;
    auto typeValueOf(const ast::Expr& expr) const -> std::optional<semantic::StructuredType>;
    auto receiverTypeOf(const ast::Expr& call) const -> std::string;
    auto typeOfAnswer(const ast::MethodCall& call) const
        -> const semantic::StaticTypeAnswer*;
    auto selectedSignatureOf(const ast::Expr& call) const -> const semantic::Signature*;
    auto shownThroughOf(const ast::Expr& call, std::size_t index) const -> const std::string*;
    auto inheritedDefaultsOf(const ast::MakeDef& make) const
        -> const std::vector<semantic::InheritedDefault>*;
    auto destructuresString(const ast::Pattern& pattern) const -> bool;

private:
    std::vector<const Program*> m_programs;
    const Program* m_base = nullptr;
    // Which pushed program answers for each node, the newest when several do:
    // a lookup asks that one, then the base, instead of every program in turn.
    std::unordered_map<const void*, const Program*> m_owners;
};

// What is not yet decided: one line per undecided call site (`--verify-hir`).
auto verify(const Program& program) -> std::vector<std::string>;

} // namespace kex::hir

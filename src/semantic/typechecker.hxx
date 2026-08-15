#pragma once

#include "../ast/ast.hxx"
#include "imported_interfaces.hxx"
#include "symbol.hxx"
#include "traits.hxx"
#include "types.hxx"
#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

struct Diagnostic;

class TypeChecker {
public:
    explicit TypeChecker(const ImportedInterfaces* importedInterfaces = nullptr)
        : m_importedInterfaces(importedInterfaces) {}

    auto check(const ast::Program& program, std::vector<Diagnostic>& diagnostics) -> void;

    // Query the inferred type of any expression node after check() has run.
    // Returns nullptr for nodes not visited (e.g. unreachable code).
    auto typeOf(const ast::Expr* expr) const -> TypePtr;
    // `typeOf` widened for presentation: a nullary constructor's type is its
    // own name (`Red`), but what a reader wants to see is the ADT it belongs
    // to (`Colour`). Checking is unaffected — this is display only, and it is
    // the same constructor→owner relation `satisfiesTrait` uses.
    auto displayTypeOf(const ast::Expr* expr) const -> TypePtr;
    auto typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>&;
    auto isTrait(const std::string& name) const -> bool {
        return m_traits.get(name) != nullptr;
    }
    auto traitRequires(const std::string& trait,
                       const std::string& method) const -> bool {
        const auto* definition = m_traits.get(trait);
        if (!definition) return false;
        return std::any_of(
            definition->requiredMethods.begin(),
            definition->requiredMethods.end(),
            [&](const auto& required) { return required.name == method; });
    }
    auto traitNeedsDictionary(const std::string& trait) const -> bool {
        const auto* definition = m_traits.get(trait);
        return definition && !definition->requiredMethods.empty();
    }
    auto functionSignatures(const ast::FunctionDef* function) const
        -> const std::vector<Signature>*;
    // Canonical source-facing rendering. Internal inference variable IDs are
    // deliberately normalized to A, B, C, ... for diagnostics and tooling.
    auto displaySignature(const std::string& name,
                          const Signature& sig) const -> std::string;
    auto resolvedCalls() const
        -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>&;
    auto selectedCallSignatures() const
        -> const std::unordered_map<const ast::Expr*, Signature>& {
        return m_selectedCallSignatures;
    }
    // `Type.of(x)` call sites whose argument the checker typed concretely.
    // Both backends materialize the recorded shape instead of asking the
    // value, which is what lets `Type.of` see a Result's unused half and an
    // empty list's element type. Absent entry (or no analysis at all, as with
    // `--no-check`) means "ask the value".
    auto staticTypeOfCalls() const
        -> const std::unordered_map<const ast::MethodCall*, StaticTypeAnswer>& {
        return m_staticTypeOfCalls;
    }
    auto referencedModules() const
        -> const std::unordered_set<std::string>& {
        return m_referencedModules;
    }

private:
    // Top-level
    auto checkTopLevel(const ast::TopLevelItem& item) -> void;
    auto checkModule(const ast::ModuleDef& mod) -> void;
    auto checkUsingBlock(const ast::UsingBlock& block) -> void;

    // ADT registry (sum types with a known, closed constructor set), for
    // match exhaustiveness checking. Populated in a pass over the whole
    // program before any function body is checked, so forward references
    // work the same way `m_globals` already does.
    auto registerAdt(const ast::TypeDef& def) -> void;
    auto registerAdtsInModule(const ast::ModuleDef& mod) -> void;

    // Type alias registry: `type X = <type_expr>` where the RHS is not
    // constructor-shaped (no UpperIdent variants) — the aliased type is
    // expanded inline whenever X appears in a type annotation.
    auto registerTypeAliases(const ast::Program& program) -> void;
    auto registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void;
    auto typeDefToType(const ast::TypeDef& def) -> TypePtr;

    // Standalone type signatures (`fact : Integer -> Integer`) — registered
    // before function bodies are checked so the annotation acts as a declared
    // contract rather than competing with body inference.
    auto registerDeclaredSignatures(const ast::Program& program) -> void;
    auto registerDeclaredSignaturesInModule(
        const ast::ModuleDef& mod, const std::string& parentPath = "") -> void;
    auto annotationToSignature(
        const ast::TypeAnnotation& ann,
        std::unordered_map<std::string, TypePtr>* genericVars = nullptr)
        -> std::optional<Signature>;
    auto registerMakeSignatures(const ast::Program& program) -> void;
    auto registerMakeSignaturesInModule(const ast::ModuleDef& mod,
                                       const std::string& parentPath) -> void;
    auto registerMakeSignature(const ast::MakeDef& def,
                               const std::string& modulePath) -> void;
    auto makeModuleVisible(const std::string& module) const -> bool;
    auto reportUnknownMethods() -> void;
    auto publishQualifiedSignatures(const std::string& name,
                                    const std::vector<Signature>& signatures) -> void;
    std::unordered_set<std::string> m_qualifiedPublished;
    // Method calls that resolved to nothing, judged after the whole program is
    // registered (a make block below the call registers its methods later).
    struct UnresolvedMethod {
        std::string name;
        SourceLocation location;
        std::string receiver;
    };
    std::vector<UnresolvedMethod> m_unresolvedMethods;
    // Every name any `make` block defines, private methods included.
    std::unordered_set<std::string> m_makeMethodNames;

    // Pre-register provisional signatures (param types from inline annotations,
    // TypeVar result) for all non-annotation-declared FunctionDefs before any
    // body is checked. This makes recursive and forward-reference calls
    // checkable: a call to `fact` inside `fact`'s own body, or a call to `b`
    // defined after `a`, finds a signature in m_userSignatures.
    auto preRegisterFunctionSigs(const ast::Program& program) -> void;
    auto preRegisterFunctionDef(const ast::FunctionDef& def) -> void;
    auto checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> void;

    // Defines every variable a pattern introduces (VarPattern, shorthand
    // record fields, nested constructor/list/tuple args) in the current
    // scope as a fresh type var — mirrors how checkFunctionDef already
    // treats untyped params, just recursing into pattern structure.
    auto bindPatternVars(
        const ast::Pattern& pat, TypePtr expected = nullptr) -> void;
    auto registerTraits(const ast::Program& program) -> void;
    auto checkFunctionDef(const ast::FunctionDef& def) -> void;
    auto checkMakeDef(const ast::MakeDef& def) -> void;
    auto checkTraitImplementation(const ast::MakeDef& def) -> void;
    auto checkMainBlock(const ast::MainBlock& block) -> void;

    // Resolves a parsed `ast::TypeExpr` (param/signature annotation syntax)
    // into a semantic::Type. `genericVars` is scoped to one function clause
    // so repeated occurrences of the same single-letter generic name (e.g.
    // `zip(a : [A], b : [B])`) resolve to the same TypeVar within that
    // clause, not a fresh one per occurrence.
    auto resolveTypeExpr(const ast::TypeExpr& typeExpr,
                        std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr;

    // Type inference for expressions
    auto inferExpr(const ast::Expr& expr) -> TypePtr;
    auto inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr;

    // Infer a trailing block (Lambda or BlockExpr) with optional hint param
    // types so the call site constrains the lambda's params before the body
    // is checked. Returns a FuncType wrapping inferred param and body types.
    auto inferBlock(const ast::Expr& blockExpr,
                    const std::vector<TypePtr>& hintParams) -> TypePtr;

    // Look up matching signatures for `name` with `nonBlockArgTypes` as the
    // non-block args, find the first that has a FuncType as its last param,
    // and resolve any negative-ID generic placeholders against the actual arg
    // types. Returns the resolved block param types, or empty if not found.
    auto resolveBlockHints(const std::string& name,
                           const std::vector<TypePtr>& nonBlockArgTypes,
                           bool isMethodCall = false) -> std::vector<TypePtr>;
    // Like resolveBlockHints but for an inline lambda at a specific arg index.
    // Returns the FuncType param hints for that position, applying generic substitution
    // from the other (already-known) arg types.
    auto resolveArgHints(const std::string& name,
                         const std::vector<TypePtr>& argTypes,
                         size_t slArgIdx,
                         bool isMethodCall = false) -> std::vector<TypePtr>;

    // Signatures of `name` visible through the imported package interfaces:
    // receiver functions for bare names, qualified module exports for
    // `Module::name`, and automatic-import module exports otherwise.
    auto importedCandidateSignatures(const std::string& name) const
        -> std::vector<Signature>;
    auto importedFunctionVisible(const ImportedFunction& function) const -> bool;
    auto moduleMemberImported(const std::string& module,
                              const std::string& member) const -> bool;

    // Binary operator type resolution
    auto inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                       SourceLocation loc) -> TypePtr;

    // Call checking (FunctionCall and MethodCall, the latter desugared to
    // the same path with the receiver prepended as the first argument).
    // Resolves `name` against imported package interfaces and the user-defined
    // top-level/module-level function table (m_userSignatures) — not
    // make-block methods, whose implicit `this` receiver isn't a regular
    // param, so the "receiver is argument 0" UFCS desugaring used here
    // would mis-count arity for them (see checkMakeDef).
    auto checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                   SourceLocation loc, bool isMethodCall = false,
                   const ast::MethodCall* methodCall = nullptr,
                   const ast::Expr* callExpr = nullptr) -> TypePtr;
    auto argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool;
    auto resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr;
    auto namedFunctionSignature(const ast::Expr& expr) -> const Signature*;
    auto typeNameReference(const ast::Expr& expr) -> TypePtr;
    // Backing store for the pointer namedFunctionSignature returns when the
    // match came from imported interfaces.
    std::vector<Signature> m_importedSignatureCache;
    // `type X = Type.returnedBy(f)` declarations, held until the builtin type
    // names exist.
    std::vector<std::pair<std::string, const ast::TypeQuery*>> m_computedAliases;
    // Declared functions by name, so `type X = Type.returnedBy(f)` can read a
    // return annotation before signature registration has run.
    std::unordered_multimap<std::string, const ast::FunctionDef*>
        m_functionDeclarations;
    auto declaredBindingType(const std::optional<ast::TypeExprPtr>& annotation,
                             const TypePtr& valueType, SourceLocation loc) -> TypePtr;
    auto declaredConstantType(const std::string& name, const TypePtr& valueType,
                              SourceLocation loc) -> TypePtr;
    bool m_inSyntheticMain = false;
    // Trait conformance for a value type, lifting a nullary ADT constructor
    // (`Dog`) to the ADT that declares the conformance (`Animal`).
    auto satisfiesTrait(const TypePtr& type, const std::string& traitName) const -> bool;
    // Scope management
    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineVar(const std::string& name, TypePtr type) -> void;
    auto lookupVar(const std::string& name) const -> TypePtr;

    // Error reporting
    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto typeMismatch(SourceLocation loc, const TypePtr& expected, const TypePtr& actual) -> void;

    std::vector<Diagnostic>* m_diagnostics = nullptr;
    TypeEnv m_globals;
    std::vector<TypeEnv> m_scopeStack;
    struct ImportSelection {
        std::string module;
        std::vector<std::string> onlyNames;
        std::vector<std::string> exceptNames;
    };
    std::vector<std::vector<ImportSelection>> m_importScopeStack;
    std::vector<ImportSelection> m_declarationImports;
    std::unordered_map<const ast::FunctionDef*, std::vector<ImportSelection>>
        m_functionImports;
    std::unordered_map<const ast::MakeDef*, std::vector<ImportSelection>>
        m_makeImports;
    std::unordered_map<const ast::MainBlock*, std::vector<ImportSelection>>
        m_mainImports;
    int m_nextTypeVar = 0;
    TraitRegistry m_traits = TraitRegistry::withBuiltins();
    const ImportedInterfaces* m_importedInterfaces = nullptr;
    std::unordered_map<const ast::MethodCall*, ResolvedCallTarget> m_resolvedCalls;
    std::unordered_map<const ast::Expr*, Signature> m_selectedCallSignatures;
    std::unordered_map<const ast::MethodCall*, StaticTypeAnswer> m_staticTypeOfCalls;
    std::unordered_set<std::string> m_referencedModules;
    // Source module identities declared by the current compilation unit.
    // Local modules take precedence over package interfaces with the same name.
    std::unordered_set<std::string> m_localModules;
    struct ModuleConstructor {
        std::string typeName;
        size_t arity = 0;
        bool isPublic = true;
    };
    std::unordered_map<std::string,
        std::unordered_map<std::string, ModuleConstructor>>
        m_moduleConstructors;

    // typeName -> constructor names; constructorName -> owning typeName.
    std::unordered_map<std::string, std::vector<std::string>> m_adtVariants;
    std::unordered_map<std::string, std::string> m_adtOfConstructor;
    // What applying a constructor produces. `slots[i]` is the index of the
    // ADT type parameter the i-th payload IS (`Just(X)` → slot 0), or -1 when
    // the payload is some other type expression and tells us nothing about
    // the type arguments.
    struct ConstructorResult {
        std::string adtName;
        std::size_t typeParamCount = 0;
        std::vector<int> slots;
        // The payload types as DECLARED (`Boxed(Box)` -> [Box]). `slots` only
        // says which payloads are type parameters; a payload naming a concrete
        // type was previously discarded, so destructuring it in a pattern gave
        // the binding a fresh variable and nothing knew `b` was a Box.
        std::vector<TypePtr> payloadTypes;
    };
    std::unordered_map<std::string, ConstructorResult> m_constructorResult;
    // Payload count per ADT constructor (`Just` -> 1, `None` -> 0), used to
    // reject a pattern that destructures the wrong number of values.
    std::unordered_map<std::string, size_t> m_constructorArity;
    // Make-block methods that carry a `:>` declaration. That declaration is
    // authoritative, so the inferred signature of its `let` must not be
    // registered alongside it as a second, permissive overload.
    std::unordered_set<std::string> m_annotatedMethods;
    auto checkPatternArity(const ast::Pattern& pattern) -> void;
    // The ADT a scrutinee type belongs to ("Optional", "Result", …), or ""
    // when the type is not a closed ADT (a type variable, Any, a record).
    auto adtNameOfType(const TypePtr& type) const -> std::string;
    // Whether an expression always exits the enclosing function, so the
    // match arm / branch it forms hands no value to its surroundings.
    static auto alwaysReturns(const ast::Expr& expr) -> bool;
    static auto alwaysReturns(const std::vector<ast::ExprPtr>& body) -> bool;
    // The type `Ctor(args...)` produces, or nullptr when the name is not a
    // registered ADT constructor.
    auto constructorResultType(const std::string& name,
                               const std::vector<TypePtr>& argTypes)
        -> TypePtr;
    // Errors when a constructor pattern names an arm of a DIFFERENT ADT than
    // the value being matched — `if let Ok(x) = anOptional`.
    auto checkPatternConstructorOwner(const ast::Pattern& pattern,
                                      const TypePtr& expected) -> void;
    std::unordered_set<std::string> m_nullaryConstructors;

    // Record field types: typeName -> { fieldName -> TypePtr }.
    // Populated by a pre-pass over all RecordDefs so field access in method
    // chains (`user.name`, `point.x`) returns the declared field type.
    std::unordered_map<std::string,
                       std::unordered_map<std::string, TypePtr>> m_recordFields;
    auto registerRecordFields(const ast::Program& program,
                              bool namesOnly = false) -> void;
    auto resolveRecordName(const std::string& name) const -> std::string;

    // Type alias map — populated before function bodies are checked.
    std::unordered_map<std::string, TypePtr> m_typeAliases;

    // Functions whose signatures came from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — these are "declared" and checkFunctionDef
    // uses them to validate the body.  Pre-registered provisional sigs (for
    // forward-reference/recursion support) are NOT in this set, so they
    // don't accidentally gate body param-type overrides.
    std::set<std::string> m_annotationDeclared;
    // Exact lexical owner -> declared contracts. The unqualified call table
    // remains separate; this map prevents `A.get`'s annotation from being
    // used to validate `B.get` merely because their source names coincide.
    std::unordered_map<std::string, std::vector<Signature>>
        m_scopedDeclaredSignatures;

    // Per-clause signatures for top-level/module-level user functions,
    // built as each FunctionDef is checked — so a call to a function
    // defined earlier in the file gets real argument checking, the same
    // way the existing forward-reference limit already applies uniformly
    // (a call to a function defined *later*, or a self/mutually-recursive
    // call within a not-yet-finished FunctionDef, isn't in here yet and
    // checkCall falls back to Type::unknown() for it — no regression vs.
    // today, just not yet improved; that's call-graph SCC ordering,
    // phase 5b, not attempted here).
    std::unordered_map<std::string, std::vector<Signature>> m_userSignatures;
    // Receiver-aware signatures declared inside `make T do` blocks. Their
    // first parameter is the implicit receiver, matching UFCS call checking.
    std::unordered_map<std::string, std::vector<Signature>> m_methodSignatures;
    // Checked interface attached to its exact syntax declaration. Unlike the
    // call-resolution table above, this preserves ownership when separate
    // modules or overload declarations reuse the same unqualified name.
    std::unordered_map<const ast::FunctionDef*, std::vector<Signature>>
        m_functionSignatures;
    // Names whose provisional pre-registration has been replaced by the first
    // real checkFunctionDef call. Subsequent calls for the same name append
    // rather than replace, building the overload set incrementally.
    std::set<std::string> m_checkedFunctions;
    // Purity is a property of an overload set, not an individual candidate:
    // dispatch must never decide whether a call is foul.
    std::unordered_map<std::string, bool> m_overloadPurity;
    // Lexical module qualification keeps same-named overload sets independent.
    std::string m_currentModulePath;
    std::unordered_map<std::string, std::set<size_t>> m_annotationArities;
    bool m_inMakeBlock = false;
    // How deeply the current expression sits inside blocks/lambdas, and how
    // many `.try`s have been seen below the block depth a `trying` body
    // started at. A block is a function boundary, so a `.try` inside one
    // never reaches the enclosing rescue — see the TryingExpr case.
    int m_blockDepth = 0;
    int m_tryingBlockDepth = -1;
    int m_tryBelowBlock = 0;
    // The complete receiver type of the current `make X do` block, used for
    // `this`, `This`, and receiver-aware body inference.
    TypePtr m_currentMakeType;
    // Module owning the `make` block being checked, "" at top level.
    std::string m_currentMakeModule;

    // Populated by inferExpr; maps each visited Expr node to its inferred type.
    std::unordered_map<const ast::Expr*, TypePtr> m_typeMap;

    // Unification substitution: TypeVar id → the concrete/constrained type it
    // was unified with during body inference. Never cleared between functions
    // because TypeVar IDs are globally unique (monotonically incrementing).
    std::unordered_map<int, TypePtr> m_subst;

    // Walk m_subst to find the concrete type behind a TypeVar, following any
    // chain of substitutions. Returns `t` unchanged for non-TypeVar types.
    auto resolve(TypePtr t) const -> TypePtr;

    // Record that TypeVar `id` equals `concrete`.
    // No-op if `id` already has a binding (first writer wins).
    auto unifyVar(int id, TypePtr concrete) -> void;

    auto freshTypeVar() -> TypePtr;
};

} // namespace kex::semantic

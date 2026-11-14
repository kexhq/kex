#pragma once

#include "../ast/ast.hxx"
#include "decided_call.hxx"
#include "declared_type.hxx"
#include "intrinsics.hxx"
#include "imported_interfaces.hxx"
#include "traits.hxx"
#include "type_test.hxx"
#include "types.hxx"
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

struct Diagnostic;

class ModuleSymbols;

class TypeChecker {
public:
    explicit TypeChecker(const ImportedInterfaces* importedInterfaces = nullptr)
        : m_importedInterfaces(importedInterfaces) {}

    // `entryFiles`: the program's own files (Analyzer::setEntryFiles); what
    // any other merged file declares at its top level is that file's own.
    auto check(const ast::Program& program, std::vector<Diagnostic>& diagnostics,
               const std::vector<std::string>& entryFiles = {}) -> void;
    auto linkModules(std::set<std::string> modules) -> void {
        m_linkedModules = std::move(modules);
    }

    // Query the inferred type of any expression node after check() has run.
    // Returns nullptr for nodes not visited (e.g. unreachable code).
    auto typeOf(const ast::Expr* expr) const -> TypePtr;
    // `typeOf` widened for presentation: a nullary constructor's type is its
    // own name (`Red`), but what a reader wants to see is the ADT it belongs
    // to (`Colour`). Checking is unaffected — this is display only, and it is
    // the same constructor→owner relation `satisfiesTrait` uses.
    auto displayTypeOf(const ast::Expr* expr) const -> TypePtr;
    auto typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>&;
    // The type whose implementation answers a trait dictionary for `value`,
    // as printed: the ADT for a constructor's value; "" for a value known
    // only by a trait or a type variable, which its own type answers when it
    // runs.
    auto dictionaryTypeOf(const ast::Expr& value) const -> std::string;
    // The trait a declared parameter type names, or "".
    auto declaredTraitOf(const TypePtr& type) const -> std::string;
    // The binding being mutated by an assignment. Assignment expressions
    // themselves are Void, but tooling still needs the receiver type for
    // hover and member completion on `binding.field = value`.
    auto assignmentTargetTypeOf(const ast::Expr* expr) const -> TypePtr;

    // Names introduced by a PATTERN, with where they were written. Patterns
    // are not expressions, so nothing about `Boxed(b)`'s `b` appears in
    // typeMap and an editor had nothing to say about the binding itself —
    // only about later uses of it.
    struct PatternBinding {
        std::string name;
        SourceLocation location;
        TypePtr type;
    };
    auto patternBindings() const -> const std::vector<PatternBinding>& {
        return m_patternBindings;
    }

    // One `using`: its module as written, and what it brings in.
    struct ImportSelection {
        std::string module;
        std::vector<std::string> onlyNames;
        std::vector<std::string> exceptNames;
        // `using Module, as: Alias`'s alias, so a qualified reference
        // written through it (`Alias.Member`) can be resolved back to
        // `Module.Member` — the identity every other lookup (record fields,
        // method ownership, ...) is keyed by.
        std::optional<std::string> alias;
        // Where it is written.
        SourceLocation location;
    };
    // What tooling reads of a program's names (name_index.hxx), kept only
    // when asked for (setRecordsNames): a build has no use for it.
    // The module an expression is written in and the `using`s in force there.
    struct ScopeContext {
        std::string module;
        std::vector<ImportSelection> imports;
    };
    // The names bound around an expression, and its context.
    struct ScopeNames {
        std::vector<std::string> bound;
        std::size_t context = 0;
    };
    auto setRecordsNames(bool records) -> void { m_recordsNames = records; }
    // Each expression that names something no scope binds (an identifier, a
    // bare call, a constructor, a tag, a capture), with its context.
    auto nameUses() const -> const std::unordered_map<const ast::Expr*, std::size_t>& {
        return m_nameUses;
    }
    auto scopeNames() const -> const std::unordered_map<const ast::Expr*, ScopeNames>& {
        return m_scopeNames;
    }
    auto scopeContexts() const -> const std::vector<ScopeContext>& { return m_scopeContexts; }
    // Where the local each name use (an Identifier) means is bound, and the
    // places locals are bound: what go-to-definition answers a local with.
    auto localDefinitions() const -> const std::unordered_map<const ast::Expr*, SourceLocation>& {
        return m_localDefinitions;
    }
    auto bindingSites() const -> const std::vector<std::pair<std::string, SourceLocation>>& {
        return m_bindingSites;
    }
    // What a member completion may offer after a value of `receiver` written
    // where `at` is (an expression whose names were recorded): each name the
    // checker's tables hold for which a call there — `receiver.name(...)`,
    // checked by checkCall under the module and `using`s in force at `at` —
    // is accepted, with the signature it was probed at and the result the
    // call would have. The checker's own answer, not a second lookup by name:
    // an offer is exactly what a check of the call would accept. Runs after
    // the check and leaves no trace of the probes (diagnostics, deferred
    // method reports, bindings).
    struct MemberCompletion {
        std::string name;
        Signature signature;
        TypePtr result;
        bool field = false;
        // A serving slot, called on the Server<State> that runs it.
        bool slot = false;
    };
    auto memberCompletions(const TypePtr& receiver, const ast::Expr& at)
        -> std::vector<MemberCompletion>;
    // What a completion of a bare name may offer where `at` is: the names
    // bound around it and every name the checker's tables hold that a bare
    // use there reaches (bareNameDeclared, under the module and `using`s in
    // force at `at`). Lowercase locals first is the caller's to decide.
    auto bareCompletions(const ast::Expr& at) -> std::vector<std::string>;
    // What `.try` on a value of `operand` yields: an Optional's or a Result's
    // success payload; null when `.try` does not apply.
    auto tryPayload(const TypePtr& operand) const -> TypePtr;
    // What `candidates` take between them at argument `position`: the union
    // of their parameter types there, or null when one of them is open
    // (generic) there and so takes anything (kexhq/kex#643).
    // The branch type a conditional takes when one branch is known only in
    // part (`[]`, a list of unknown elements) and the next one fully, and
    // fits it: the known one. Null otherwise. Kept as `[Unknown]`, `if c
    // then [] else xs.map {..}` let every later use of the list through.
    auto moreKnownBranch(const TypePtr& held, const TypePtr& next) const -> TypePtr;
    auto overloadsTake(const std::vector<const Signature*>& candidates,
                       std::size_t position) const -> TypePtr;
    // The result of a call none of `candidates` takes whole, when an argument
    // is a union each of whose members one of them takes; none otherwise
    // (kexhq/kex#647).
    auto unionArgumentCovered(const std::vector<const Signature*>& candidates,
                              const std::vector<TypePtr>& argTypes) const
        -> std::optional<TypePtr>;
    // The list patterns that take a String apart into its characters
    // (`[c | rest]` against a String): the elements bind Chars, the tail the
    // rest of the String. Pattern sugar, decided here; a String is no list.
    // The clauses with no declared result whose `.try` can fail, by their last
    // body expression: the constructor their successful value is wrapped in.
    auto successWrappers() const -> const std::unordered_map<const ast::Expr*, std::string>& {
        return m_successWrappers;
    }
    auto stringPatterns() const -> const std::unordered_set<const ast::Pattern*>& {
        return m_stringPatterns;
    }
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
    // Does the trait declare this required method `foul`? A trait-typed
    // receiver dispatches dynamically, so such a call has no single resolved
    // target — the trait declaration is the only place its effect is stated.
    auto traitMethodIsFoul(const std::string& trait,
                           const std::string& method) const -> bool {
        const auto* definition = m_traits.get(trait);
        if (!definition) return false;
        return std::any_of(
            definition->requiredMethods.begin(),
            definition->requiredMethods.end(),
            [&](const auto& required) {
                return required.name == method && required.isFoul;
            });
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
    auto resolvedTraits() const
        -> const std::unordered_map<const ast::MakeDef*,
                                    std::vector<std::string>>& {
        return m_resolvedTraits;
    }
    auto selectedCallSignatures() const
        -> const std::unordered_map<const ast::Expr*, Signature>& {
        return m_selectedCallSignatures;
    }
    // Arguments of a distinct type given for a `Showable` parameter, by call:
    // each one's parameter position and the type, whose own `showValue`
    // shows it (at run time the value is its erased backing value).
    auto shownThroughDistinct() const -> const ShownThrough& { return m_shownThroughDistinct; }
    // What a call-shaped expression turned out to be, when that is neither a
    // call with a selected signature nor an imported method (the two tables
    // above). Stage 2 of the pass restructure records every such decision
    // here, so no later pass has to resolve the call again (src/hir/).
    using CallKind = semantic::CallKind;
    using DecidedCall = semantic::DecidedCall;
    auto decidedCalls() const
        -> const std::unordered_map<const ast::Expr*, DecidedCall>& {
        return m_decidedCalls;
    }
    // The parameters written as a bare type name, which take that type as a
    // value (typeSelectorParameter): `let to(String)`.
    auto typeSelectorPatterns() const -> const std::unordered_set<const ast::Pattern*>& {
        return m_typeSelectorPatterns;
    }
    // The trait each parameter declared as one names, which the parameter
    // takes a dictionary for.
    auto parameterTraits() const
        -> const std::unordered_map<const ast::Param*, std::string>& {
        return m_parameterTraits;
    }
    // The trait a type spelling written in module `scope` names, or "".
    auto traitWrittenAs(const std::string& scope, const std::string& written) const
        -> std::string;
    // An annotation's declared type, for run-time reflection (`Type.fields`):
    // as written, with the program's transparent aliases followed.
    auto declaredTypeOf(const ast::TypeExpr& type) const -> StructuredType;
    auto expandDeclared(StructuredType type) const -> StructuredType;
    // An annotation as the run-time dispatchers test it: the record it was
    // resolved to, else as written.
    auto annotationSpelling(const ast::TypeExpr& type) const -> std::string;
    // A standalone contract as the checker resolved it, or nullptr.
    auto quantifiedContract(const Signature& contract) const -> Signature;
    // A record field's type as resolved when the record was registered, its
    // parameters as slots (-1 for the first); null when there is none.
    auto recordFieldType(const std::string& record, const std::string& field) const -> TypePtr {
        const auto found = m_recordFields.find(record);
        if (found == m_recordFields.end()) return nullptr;
        const auto type = found->second.find(field);
        return type == found->second.end() ? nullptr : type->second;
    }
    // The same with the record's parameters by their declared names
    // (`value : V`), as an editor shows it.
    auto recordFieldType(const std::string& record, const std::string& field,
                         const std::vector<std::string>& typeParams) const -> TypePtr;
    auto inheritedDefaults() const
        -> const std::unordered_map<const ast::MakeDef*, std::vector<InheritedDefault>>& {
        return m_inheritedDefaults;
    }
    auto contractSignatureOf(const ast::TypeAnnotation& contract) const -> const Signature* {
        const auto found = m_contractSignatures.find(&contract);
        return found == m_contractSignatures.end() ? nullptr : &found->second;
    }
    // What a type spelled `written` in module `scope` says about a value's
    // form (type_test.hxx).
    auto typeTestOf(const std::string& scope, const std::string& written) const -> TypeTest;
    auto typeIdentity(const std::string& scope, const std::string& name) const -> std::string;
    auto valueFormOf(const std::string& scope, const std::string& written) const -> TypeTest;
    auto registerDeclaredAliases(const ast::Program& program) -> void;
    // The declaration each `make` method is made for, and the record each
    // named record pattern names.
    auto calledFunctions() const -> const std::unordered_map<const ast::Expr*, std::string>& {
        return m_calledFunctions;
    }
    auto referencedConstants() const -> const std::unordered_map<const ast::Expr*, std::string>& {
        return m_referencedConstants;
    }
    // The types each `make` block is for, by identity (makeOwnerNames).
    auto makeTargetIdentities() const
        -> const std::unordered_map<const ast::MakeDef*, std::vector<std::string>>& {
        return m_makeTargetIdentities;
    }
    auto annotatedRecords() const
        -> const std::unordered_map<const ast::TypeExpr*, std::string>& {
        return m_annotatedRecords;
    }
    auto annotatedTypes() const
        -> const std::unordered_map<const ast::TypeExpr*, std::string>& {
        return m_annotatedTypes;
    }
    auto recordPatterns() const
        -> const std::unordered_map<const ast::RecordPattern*, std::string>& {
        return m_recordPatterns;
    }
    // The trait each method of a `make` for one is made for.
    auto makeTargetTraits() const
        -> const std::unordered_map<const ast::FunctionDef*, std::string>& {
        return m_makeTargetTraits;
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
    auto registerAdt(const ast::TypeDef& def, const std::string& modulePath = "") -> void;
    auto registerAdtsInModule(const ast::ModuleDef& mod) -> void;

    // Type alias registry: `type X = <type_expr>` where the RHS is not
    // constructor-shaped (no UpperIdent variants) — the aliased type is
    // expanded inline whenever X appears in a type annotation.
    auto registerTypeAliases(const ast::Program& program) -> void;
    auto registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void;
    auto typeDefToType(const ast::TypeDef& def) -> TypePtr;
    auto distinctBacking(const TypePtr& type) const -> TypePtr;
    auto resolveDistinctName(const std::string& name) const -> std::string;
    auto representationsCompatible(const TypePtr& from,
                                   const TypePtr& to) -> bool;
    auto checkRecordDef(const ast::RecordDef& def) -> void;

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
    auto noteOwnMethods(const ast::MakeDef& def, const std::string& modulePath) -> void;
    auto makeTargetKey(const TypePtr& receiver) const -> std::string;
    static auto ownMethodKey(const ast::FunctionDef& method) -> std::string;
    // Collects every `export M[, as: Alias]` in the program into
    // m_moduleReexports, keyed by the re-namespaced path it creates
    // ("App.Geometry" for `module App do export Geometry end`) mapping to
    // the module it actually names ("Geometry") — see resolveRecordName's
    // alias-resolution step, which consults this the same way it does a
    // `using M, as: Alias`.
    auto registerExports(const ast::Program& program) -> void;
    auto registerExportsInModule(const ast::ModuleDef& mod,
                                 const std::string& parentPath) -> void;
    auto registerMakeSignature(const ast::MakeDef& def,
                               const std::string& modulePath) -> void;
    auto makeModuleVisible(const std::string& module) const -> bool;
    // The module of an out-of-scope `make` whose method `name` takes these
    // arguments (receiver first), or empty: what a call no visible candidate
    // fits is `Hidden` by.
    auto hiddenMakeTaking(const std::string& name, const std::vector<TypePtr>& argTypes)
        -> std::string;
    // The type of the constant `name` names here: a member without
    // parameters of an enclosing module, innermost first, or of the top
    // level (`let LIMIT = 100`). Null when it names none.
    auto constantType(const std::string& name) const -> TypePtr;
    auto constantDeclaration(const std::string& name) const -> std::string;
    // The type of the nullary constructor `name` names here, or null.
    auto nullaryConstructorType(const std::string& name) -> TypePtr;
    auto reportConstructorOutOfScope(const std::string& name, const SourceLocation& at) -> bool;
    // The module declaring constructor `name` when only a module does and
    // its bare name is out of scope here (not inside it, no `using` brings
    // it in); "" when the bare name may be used.
    auto moduleOfUnreachableConstructor(const std::string& name) const -> std::string;
    // Refuses `name`, a constructor of `declaring` named bare out of scope.
    auto reportUnreachableConstructor(const std::string& name, const std::string& declaring,
                                      const SourceLocation& at) -> void;
    // A name nothing in scope declares: the one report of it, for every
    // consumer of the check (`what` is "name", "function", "variable"...).
    auto reportUndefined(const std::string& what, const std::string& name,
                         const SourceLocation& at) -> void;
    // Whether a free function of the program named `name` is in reach here.
    auto bareFunctionReachable(const std::string& name) const -> bool;
    // The result a body with no declared result gets from its failing
    // `.try`s (`T?` or `T or! E`), or `type` itself; `who` names the body in
    // a refusal. A function's (`isFunction`) refuses its `return`s and a mix
    // of the two kinds of `.try`; a block keeps its own type for a mix.
    auto inferredFailingResult(const std::string& who, const std::vector<ast::ExprPtr>& body,
                               const std::vector<std::pair<SourceLocation, TypePtr>>& tries,
                               const TypePtr& type, bool isFunction) -> TypePtr;
    // Each of `tries` refused where `declared` cannot hold its failure.
    auto refuseUnheldTries(const std::string& who, const TypePtr& declared,
                           const std::vector<std::pair<SourceLocation, TypePtr>>& tries) -> void;
    // A block's result after its `.try`s: inferred as above where it
    // declares none, refused where it declares one that cannot hold them.
    auto blockFailingResult(const ast::Lambda& block,
                            const std::vector<std::pair<SourceLocation, TypePtr>>& tries,
                            const TypePtr& type) -> TypePtr;
    // Whether a bare `name` no scope binds still names something here: a
    // top-level value, a function in reach, a sibling of the `make` block
    // being checked, or a function a `using` brings in.
    auto bareNameDeclared(const std::string& name) const -> bool;
    auto namesDeclaredType(const std::string& qualified, const std::string& last) const -> bool;
    auto reportUnknownType(const ast::TypeName& name, const SourceLocation& at) -> void;
    auto reportPendingUnknownTypes() -> void;
    auto spellingHasSeveralArities(const std::string& name) const -> bool;
    auto variantType(const TypePtr& result, const std::string& name) const -> TypePtr;
    auto bareMethodCall(const ast::Expr& expr, const ast::FunctionCall& call,
                        const std::vector<TypePtr>& argTypes) -> TypePtr;
    auto ownFunctionOnThis(const ast::Expr& expr, const std::string& name,
                           const std::vector<TypePtr>& argTypes) -> TypePtr;
    auto withinModule(const std::string& module) const -> bool;
    // Whether a bare call to `name` may reach a free function declared in
    // `module`: from inside it, or — unless it is private there — under a
    // `using` of it whose `only:` / `except:` admit the name.
    auto freeFunctionVisible(const std::string& module,
                             const std::string& name) const -> bool;
    // The free functions a bare `name` reaches from here (a call's or a
    // capture's); see the definition.
    // What a `~name` capture names (see the definitions).
    auto captureModule(const ast::CurryExpr& capture) const -> std::string;
    auto decideCapture(const ast::Expr& expr, const ast::CurryExpr& capture) -> void;
    auto reachableFreeFunctions(const std::string& name, bool isMethodCall,
                                std::vector<Signature>& visible,
                                std::set<std::string>& hidden) const
        -> const std::vector<Signature>*;
    // "module\nname" for each free function declared in a `private do`.
    std::unordered_set<std::string> m_privateFreeFunctions;
    auto reportUnknownMethods() -> void;
    // Decides `call` is dispatched on its value, and, when one imported module
    // in scope answers it by name and arity, files its dispatcher as the
    // resolved target.
    auto decideDispatch(const ast::Expr* call, const std::string& name) -> void;
    static auto dispatcherTarget(const ImportedFunction& dispatcher) -> ResolvedCallTarget;
    auto importedFunctionChoice(const ast::MethodCall& call, const std::string& name)
        -> std::optional<ResolvedCallTarget>;
    auto publishQualifiedSignatures(const std::string& name,
                                    const std::vector<Signature>& signatures) -> void;
    std::unordered_set<std::string> m_qualifiedPublished;
    // Method calls that resolved to nothing, judged after the whole program is
    // registered (a make block below the call registers its methods later).
    struct UnresolvedMethod {
        std::string name;
        SourceLocation location;
        std::string receiver;
        // False when the receiver is a type whose fields are fully known (a
        // primitive, an ADT, a record already searched): a field of some
        // OTHER record with this name then says nothing about the call.
        bool fieldMayExist = true;
    };
    std::vector<UnresolvedMethod> m_unresolvedMethods;
    // Types an unknown receiver was bound to by a method's name alone.
    std::unordered_set<std::string> m_guessedReceiverTypes;
    // Every name any `make` block defines, private methods included.
    std::unordered_set<std::string> m_makeMethodNames;
    // The receiver types each of those names answers on, recorded when the
    // block is registered — before the method's own signature exists. Call
    // sites inside the block reach siblings defined lower in it through this
    // (kexhq/kex#292).
    std::unordered_map<std::string, std::vector<TypePtr>> m_makeMethodReceivers;

    // Pre-register provisional signatures (param types from inline annotations,
    // TypeVar result) for all non-annotation-declared FunctionDefs before any
    // body is checked. This makes recursive and forward-reference calls
    // checkable: a call to `fact` inside `fact`'s own body, or a call to `b`
    // defined after `a`, finds a signature in m_userSignatures.
    auto preRegisterFunctionSigs(const ast::Program& program) -> void;
    auto preRegisterFunctionDef(const ast::FunctionDef& def) -> void;
    // Each answers whether it reached a verdict on the match (reported, or
    // proved it whole); what neither decides is the pattern matrix's.
    auto checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> bool;
    auto checkUnionCoverage(const ast::MatchExpr& node, const TypePtr& subject,
                            SourceLocation loc) -> bool;
    auto checkPatternMatrix(const ast::MatchExpr& node, const TypePtr& subject,
                            SourceLocation loc) -> void;

    // Defines every variable a pattern introduces (VarPattern, shorthand
    // record fields, nested constructor/list/tuple args) in the current
    // scope as a fresh type var — mirrors how checkFunctionDef already
    // treats untyped params, just recursing into pattern structure.
    auto bindPatternVars(
        const ast::Pattern& pat, TypePtr expected = nullptr) -> void;
    // A destructuring `let`/`var` against its value's type: arity first,
    // then an early constructor-mismatch error (so `var` and `let` agree),
    // then the bindings a `match` arm gets from its subject.
    auto bindDestructuredPattern(
        const ast::Pattern& pat, const TypePtr& valueType) -> void;
    auto registerTraits(const ast::Program& program) -> void;
    auto registerCapabilityTraits(const ast::Program& program) -> void;
    // Qualified names of every module declared `capability`. `with` may only
    // name one of these, which is what keeps the substitutable vocabulary
    // closed and enumerable (kexhq/kex#180).
    std::unordered_set<std::string> m_capabilities;
    auto registerLocalConformances(const ast::Program& program) -> void;
    auto checkFunctionDef(const ast::FunctionDef& def) -> void;
    auto checkMakeDef(const ast::MakeDef& def) -> void;
    // A make function redefining what an import already gives its target
    // type at that arity (kexhq/kex#637).
    auto rejectRedefinedImports(const ast::MakeDef& def) -> void;
    // Whether a clause declared to return a Result ends in its success value,
    // which is then wrapped in `Ok` (recorded as the clause's success wrapper).
    auto wrapsDeclaredSuccess(const std::vector<ast::ExprPtr>& exprs, const TypePtr& declared,
                              const TypePtr& body) -> bool;
    auto checkTraitDef(const ast::TraitDef& def) -> void;
    auto checkTraitImplementation(const ast::MakeDef& def) -> void;
    auto checkMainBlock(const ast::MainBlock& block) -> void;

    // Resolves a parsed `ast::TypeExpr` (param/signature annotation syntax)
    // into a semantic::Type. `genericVars` is scoped to one function clause
    // so repeated occurrences of the same single-letter generic name (e.g.
    // `zip(a : [A], b : [B])`) resolve to the same TypeVar within that
    // clause, not a fresh one per occurrence.
    auto resolveTypeExpr(const ast::TypeExpr& typeExpr,
                        std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr;
    auto resolveTypeExprNode(const ast::TypeExpr& typeExpr,
                             std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr;
    auto normalizeIntersection(TypePtr type) const -> TypePtr;
    // Imported record field types arrive from KexI with alias names left as
    // written (`FS.FilePath`); the local path expands them via m_typeAliases.
    // Without this the same declaration compared unequal across the module
    // boundary and record construction reported `{FS.FilePath: String}` vs
    // `{String: String}`.
    auto expandTypeAliases(const TypePtr& type, int depth = 0) const -> TypePtr;

    // Type inference for expressions
    auto inferExpr(const ast::Expr& expr) -> TypePtr;
    auto inferBody(const std::vector<ast::ExprPtr>& body, const TypePtr& expected = nullptr)
        -> TypePtr;

    // Infer a trailing block (Lambda or BlockExpr) with optional hint param
    // types so the call site constrains the lambda's params before the body
    // is checked. Returns a FuncType wrapping inferred param and body types.
    auto inferBlock(const ast::Expr& blockExpr,
                    const std::vector<TypePtr>& hintParams) -> TypePtr;

    // Look up matching signatures for `name` with `nonBlockArgTypes` as the
    // non-block args, take those that have a FuncType as their last param,
    // and resolve any negative-ID generic placeholders against the actual arg
    // types. Returns the block param types they agree on, or empty if none.
    // `skippedTo` receives the block parameter's name when every candidate
    // reaches it only by skipping defaulted parameters (kexhq/kex#554).
    auto resolveBlockHints(const std::string& name,
                           const std::vector<TypePtr>& nonBlockArgTypes,
                           bool isMethodCall = false,
                           bool* collection = nullptr,
                           std::string* skippedTo = nullptr,
                           std::optional<std::size_t> writtenParams = std::nullopt)
        -> std::vector<TypePtr>;
    // The parameter types a block passed by label (`named(from: { |t| … })`)
    // should take: those of the labelled parameter's function type.
    auto namedArgHints(const std::string& name, const std::string& label)
        -> std::vector<TypePtr>;
    // Like resolveBlockHints but for an inline lambda at a specific arg index.
    // Returns the FuncType param hints for that position, applying generic substitution
    // from the other (already-known) arg types.
    auto resolveArgHints(const std::string& name,
                         const std::vector<TypePtr>& argTypes,
                         size_t slArgIdx,
                         bool isMethodCall = false) -> std::vector<TypePtr>;
    // The hints every candidate signature agrees on, position by position;
    // Unknown where they differ (see its definition).
    auto agreedHints(const std::vector<std::vector<TypePtr>>& hintSets)
        -> std::vector<TypePtr>;
    // A variable, Unknown, or trait-bounded type: one that accepts anything.
    auto isOpenType(const TypePtr& type) -> bool;

    // Signatures of `name` visible through the imported package interfaces:
    // receiver functions for bare names, qualified module exports for
    // `Module.name`, and automatic-import module exports otherwise.
    auto importedCandidateSignatures(const std::string& name) const
        -> std::vector<Signature>;
    // The same, as the imported functions that carry them.
    auto importedCandidates(const std::string& name) const
        -> std::vector<const ImportedFunction*>;
    // The functions named `name` of the modules `using` brings in from units
    // compiled apart; `asMethod`, only those with a first parameter to be
    // the receiver.
    auto usingImportedMethods(const std::string& name, bool asMethod = true) const
        -> std::vector<const ImportedFunction*>;
    // `throughReceiver`: the call goes through a value of the function's
    // receiver type (`set.add(x)`), which reaches its own type's functions
    // without a `using`; a bare call (`add(set, x)`) does not.
    auto importedFunctionVisible(const ImportedFunction& function,
                                 bool throughReceiver = true) const -> bool;
    // Whether a `using` in scope brings `module`'s `member` in; `bare`, for
    // a call by the bare name, which `using M, as: A` alone does not.
    auto moduleMemberImported(const std::string& module, const std::string& member,
                              bool bare = false) const -> bool;

    // Binary operator type resolution
    // `==` or `!=` between an Optional (or a Result) and a settled plain
    // value, which is never equal: refused.
    auto checkNeverEqual(const TypePtr& lhs, const TypePtr& rhs, SourceLocation loc) -> void;
    // Whether `value` is an atom literal that `expected`, a set of atoms
    // (`:ok | :error`), does not admit (kexhq/kex#641).
    auto atomOutsideSet(const ast::Expr& value, const TypePtr& expected) const -> bool;
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
                   const ast::Expr* callExpr = nullptr,
                   // A call with neither node (`&.to(Float)`): its arguments,
                   // receiver slot first when `isMethodCall`.
                   const std::vector<const ast::Expr*>* argumentExprs = nullptr) -> TypePtr;
    auto argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool;
    // Whether `value` is a literal written straight into a distinct type it
    // backs: `1` as a `BookId`, `"t1"` as a `TeamId`, `[1, 2]` as `[BookId]`.
    // Fresh where the type is in view, so it needs no `.as`.
    auto literalCoercesTo(const ast::Expr& value, const TypePtr& target) const -> bool;
    // Whether `send`/`sendFrom` on this receiver is process messaging. A
    // user type with its own `send` (a WebSocket `Connection`, say) goes
    // through ordinary overload resolution instead (kexhq/kex#398).
    // Whether `name` has an EXPLICIT `:>` contract for a receiver matching
    // `receiverType` (see m_annotatedReceiverKeys). m_annotatedMethods only
    // tracks NAMES, so an unrelated type's `:>` contract (Char's
    // `string :> String`) must not stop a different type's own plain `let`
    // method of the same name (Input's `string(expected: String)`) from
    // registering — receiver, not name, is what makes two methods the same.
    // Deliberately NOT a lookup into m_methodSignatures: that table also
    // holds this same method's OWN provisional/plain-`let` registrations
    // (a type may overload one name several times, e.g. Date's `+(Duration)`
    // and `+(Period)`), so checking "any signature already there" would make
    // the first overload block every sibling one from ever registering.
    auto hasAnnotatedSignatureForReceiver(const std::string& name,
                                          const TypePtr& receiverType) const -> bool;
    // A receiver type as m_annotatedReceiverKeys files it: `Set<A>` by its
    // head, since each pass spells its variables apart; `[Integer]` whole.
    auto annotatedReceiverKey(const TypePtr& receiverType) const -> std::string;
    auto resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr;
    auto namedFunctionSignature(const ast::Expr& expr) -> const Signature*;
    // Why `written.method(...)` names nothing callable, if it does not: an
    // unknown module, or a local module without that member.
    auto namespaceCallProblem(const std::string& written, const std::string& resolved,
                              const std::string& method) -> std::optional<std::string>;
    // Whether module `path` shares its name with a type (`DateTime`,
    // `Map`): its calls then also reach that type's static functions, which
    // `make` blocks declare, so its own members are not all it answers.
    auto moduleNamesType(const std::string& path) const -> bool;
    auto typeNameReference(const ast::Expr& expr) -> TypePtr;
    // The type `name` names when written where a value or pattern goes, or
    // null when it is a constructor (`World` is a value of type `Hello`).
    auto typeNamed(const std::string& name, SourceLocation location) -> TypePtr;
    // `Type<X>` for a parameter that selects a type by name (`let to(String,
    // in: TimeUnit)`), or null for any other parameter.
    auto namesKnownType(const std::string& name) const -> bool;
    auto typeSelectorParameter(const ast::Param& param) -> TypePtr;
    // A `rescue` block's arms, each in its own scope; `onArm` gets the type of
    // every arm that does not leave the enclosing function.
    auto checkRescue(const ast::RescueBlock& rescue,
                     const std::function<void(const TypePtr&)>& onArm = {}) -> void;
    // The result of `name` on a receiver whose type claims a trait requiring
    // it, recorded as dispatched through that trait; null when none does.
    auto claimedTraitAnswers(const std::vector<TypePtr>& argTypes,
                             const std::string& name, const ast::Expr* callExpr)
        -> TypePtr;
    // Reports a call to a trait requirement with a count of values the
    // requirement does not take, receiver included; true when it did.
    auto missesRequiredArity(const Signature& required, std::size_t given,
                             const std::string& name, SourceLocation loc) -> bool;
    // Compile-time intrinsic handlers (intrinsics.hxx; typechecker_intrinsics.cxx).
    friend auto findCompileTimeIntrinsic(std::string_view, std::size_t)
        -> const CompileTimeIntrinsic*;
    auto checkAs(const ast::Expr& expr, const ast::MethodCall& call) -> TypePtr;
    auto checkTypeReturnedBy(const ast::Expr& expr, const ast::MethodCall& call)
        -> TypePtr;
    auto checkTypeOf(const ast::Expr& expr, const ast::MethodCall& call)
        -> TypePtr;
    // Who owns a method call once the overload rules chose its winner — the
    // one place a call's target is decided, so no later pass re-decides it:
    //   ReceiverType — the receiver's own type answers the method itself,
    //     and the winner is not its own module: its `make` method, or a
    //     trait default it inherits (inheriting a default makes it the
    //     type's method). The type is this program's or a unit's compiled
    //     apart, whose module holds what it inherits;
    //   Value — the receiver is erased (unknown, a type variable, `Any`) and
    //     another module answers the method too: the candidates disagree, so
    //     the value decides at run time (a sole provider still owns it);
    //   Winner — otherwise the winner's module owns it.
    enum class CallOwner { Winner, ReceiverType, Value };
    // What the receiver's own type answers a method with: its `make` block
    // (or an inherited trait default), or its home module's function over it.
    struct OwnAnswer {
        // The receiver's type, as the checker names it.
        std::string type;
        // "" for this program's types; a unit's compiled apart: its module.
        std::string home;
        // The home module's function (`Shop.Socket.TCP.close`), or "" when
        // the type's `make` block answers.
        std::string moduleFunction;
        auto decision() const -> DecidedCall {
            return moduleFunction.empty()
                ? DecidedCall{CallKind::OwnMethod, type}
                : DecidedCall{CallKind::ModuleFunction, moduleFunction};
        }
    };
    struct OwnerDecision {
        CallOwner owner = CallOwner::Winner;
        // With ReceiverType: what the type answers with.
        OwnAnswer own;
    };
    // `competing`: another module answers the method at the winner's arity.
    auto ownerOfCall(const std::vector<TypePtr>& argTypes,
                     const std::string& method,
                     const std::string& winnerModule, bool competing)
        -> OwnerDecision;
    auto ownTypeAnswers(const std::vector<TypePtr>& argTypes,
                        const std::string& method) -> std::optional<OwnAnswer>;
    auto typeHomeModule(const std::string& type) const
        -> std::optional<std::string>;
    auto ownTypeIdentity(const std::string& name) const -> std::string;
    auto foldTypeOf(const ast::Expr& expr, const ast::MethodCall& call,
                    const std::vector<TypePtr>& argTypes) -> void;
    auto checkProcessSpawn(const ast::Expr& expr, const ast::MethodCall& call) -> TypePtr;
    auto foldProcessSpawn(const ast::Expr& expr, const ast::MethodCall& call,
                          const std::vector<TypePtr>& argTypes) -> void;
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
                             const TypePtr& valueType, SourceLocation loc,
                             const ast::Expr* value = nullptr) -> TypePtr;
    auto declaredConstantType(const std::string& name, const TypePtr& valueType,
                              SourceLocation loc) -> TypePtr;
    bool m_inSyntheticMain = false;
    // Trait conformance for a value type, lifting a nullary ADT constructor
    // (`Dog`) to the ADT that declares the conformance (`Animal`).
    auto satisfiesTrait(const TypePtr& type, const std::string& traitName) const -> bool;
    // Scope management
    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineVar(const std::string& name, TypePtr type,
                   TypeEnv::Binding binding = {}) -> void;
    // A binding the program writes (`let`, `var`, a pattern's name): a name
    // the current scope already binds is a redeclaration, reported at
    // `where`; the first binding's kind is kept.
    auto declareVar(const std::string& name, TypePtr type, bool isVar,
                    SourceLocation where) -> void;
    auto lookupVar(const std::string& name) const -> TypePtr;
    // How the nearest binding of `name` was made, or null.
    auto bindingOf(const std::string& name) const -> const TypeEnv::Binding*;
    // Whether a pattern being bound is a `var`'s (set by `var` destructuring).
    bool m_patternBindsVar = false;
    // The concrete declared result of a zero-parameter binding, which a bare
    // reference to it auto-calls; nullptr when there is none (#329).
    auto zeroArgBindingResult(const std::string& name) -> TypePtr;
    // The FuncType of a bare reference to a one-or-more-parameter function
    // used as a VALUE (`check(addPair)`, no `~`); nullptr when there is no
    // single concrete signature to answer with (#350).
    auto functionValueType(const std::string& name) -> TypePtr;

    // Error reporting
    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto typeMismatch(SourceLocation loc, const TypePtr& expected, const TypePtr& actual) -> void;
    // True, and the check is left until the body answers, when `actual` is a
    // make-block method's result its body has not decided yet
    // (m_pendingResults): `main` above the make block cannot know it.
    auto deferIfPending(const TypePtr& actual, const TypePtr& expected,
                        SourceLocation loc) -> bool;
    // A pending result has its answer: bind it, then run every check that
    // was waiting for it (m_pendingChecks).
    auto answerPending(int id, const TypePtr& answer) -> void;

    std::vector<Diagnostic>* m_diagnostics = nullptr;
    // The type a typed binding asks of the conditional or `match` it is bound
    // to (`let answer : Any = match …`). Each arm is checked against it
    // rather than against the first arm (kexhq/kex#506). Taken — and so
    // cleared — by the construct it was set for, so nested branches are
    // checked as before.
    TypePtr m_branchExpectation;
    TypeEnv m_globals;
    // Analyzer::setLinkedModules.
    std::optional<std::set<std::string>> m_linkedModules;
    // The program's top-level `let name = expr` values. Functions are checked
    // before the synthetic main blocks that bind these, so a call through one
    // (`layout(live)`) has no type for it yet — but it is still the program's
    // own name, and an import of the same name must not answer the call.
    std::unordered_set<std::string> m_topLevelValueNames;
    std::vector<TypeEnv> m_scopeStack;
    std::vector<std::vector<ImportSelection>> m_importScopeStack;
    // The program's modules and their members, for what a `using` brings in.
    std::shared_ptr<const ModuleSymbols> m_programModules;
    // Reports each bare name the newest of `active` (the `using`s in force
    // in module `within`) brings in that an earlier one already did, from
    // another module (resolve_core::mergeImportOrigin).
    auto reportAmbiguousImports(const std::vector<ImportSelection>& active,
                                const std::string& within) -> void;
    // A `using` written in a body: in force for the rest of the scope.
    auto pushImport(ImportSelection selection) -> void;
    // Reports made once per place, however often the place is checked.
    std::unordered_set<std::string> m_reportedOnce;
    std::vector<std::string> m_entryFiles;
    // The functions and constructors a merged dependency's file declares
    // outside its module, by name, with that file: its own, not the program's.
    std::unordered_map<std::string, std::string> m_fileLocalTopLevel;
    // Reports a bare `name` no scope binds that only another file's top level
    // declares; false when it is not one.
    auto reportOtherFilesTopLevel(const std::string& what, const std::string& name,
                                  const SourceLocation& at) -> bool;
    // What recordNames keeps (setRecordsNames).
    bool m_recordsNames = false;
    // For tooling (setRecordsNames): where each local a name uses is bound,
    // and every place one is bound.
    std::unordered_map<const ast::Expr*, SourceLocation> m_localDefinitions;
    std::vector<std::pair<std::string, SourceLocation>> m_bindingSites;
    std::unordered_map<const ast::Expr*, std::size_t> m_nameUses;
    std::unordered_map<const ast::Expr*, ScopeNames> m_scopeNames;
    std::vector<ScopeContext> m_scopeContexts;
    std::unordered_map<std::string, std::size_t> m_scopeContextIds;
    // A module path's root (`Util` of `Util.twice()`): no use of a name.
    std::unordered_set<const ast::Expr*> m_namespaceRoots;
    auto recordNames(const ast::Expr& expr) -> void;
    auto scopeContext() -> std::size_t;
    std::vector<ImportSelection> m_declarationImports;
    // The imports in force at each of `m_unresolvedMethods`' calls, in step
    // with it: whether a method name is in reach depends on the call's own
    // file and block, not on what some other module of the program imports.
    std::vector<std::vector<ImportSelection>> m_unresolvedMethodImports;
    auto resolveTraitReference(const std::string& written, const std::string& scope,
                               const std::vector<ImportSelection>* imports,
                               bool lastResort = true,
                               std::vector<std::string>* ambiguous = nullptr) const
        -> std::string;
    std::unordered_map<const ast::FunctionDef*, std::vector<ImportSelection>>
        m_functionImports;
    std::unordered_map<const ast::MakeDef*, std::vector<ImportSelection>>
        m_makeImports;
    std::unordered_map<const ast::MainBlock*, std::vector<ImportSelection>>
        m_mainImports;
    std::unordered_map<const ast::RecordDef*, std::vector<ImportSelection>>
        m_recordImports;
    std::unordered_map<const ast::TypeDef*, std::vector<ImportSelection>>
        m_typeImports;
    // Every module path any `using` in this program names, wherever it sits.
    // Not what decides whether a method name is in reach: that is the call's
    // own imports (`m_unresolvedMethodImports`), since a union over the whole
    // program let one module's `using` reach every other file (kexhq/kex#570).
    std::unordered_set<std::string> m_importedModulePaths;
    // "App.Geometry" -> "Geometry" for `module App do export Geometry end`
    // — every `export` in the program, global rather than scope-stacked
    // like m_declarationImports/m_importScopeStack, since a re-export's
    // path is reachable from anywhere, not just where it was declared.
    // See resolveRecordName's alias-resolution step.
    std::unordered_map<std::string, std::string> m_moduleReexports;
    // `"Receiver#method"` -> the module whose `make` block declared it inside a
    // `private do ... end`. Such a method is callable only from within that
    // module; `docs/modules.md` says so, and nothing enforced it — a private
    // make method travelled with the type into every importer
    // (rodolfo docs/kex-issues.md #13, kexhq/kex#281).
    std::unordered_map<std::string, std::string> m_privateMakeMethods;
    int m_nextTypeVar = 0;
    // `m_nextTypeVar` as it stood when the clause being checked started, so a
    // variable can be told apart from one an enclosing — or an already
    // finished — inference owns. `checkCall` uses it to decide whether calling
    // a variable-typed binding may bind that variable to a function type;
    // outside any clause nothing qualifies, which is what the initial value
    // says.
    int m_clauseVarMark = std::numeric_limits<int>::max();
    TraitRegistry m_traits = TraitRegistry::withBuiltins();
    const ImportedInterfaces* m_importedInterfaces = nullptr;
    // resolveRecordName's last guess, by bare name: the imported record a
    // name is the unique suffix of, if any. It depends on nothing but the
    // interfaces, which are fixed for the checker's life, and a name that
    // nothing else claims (`String`, a type parameter) asked it on every use.
    mutable std::unordered_map<std::string, std::optional<std::string>> m_recordSuffixes;
    std::unordered_map<const ast::MethodCall*, ResolvedCallTarget> m_resolvedCalls;
    // `implement:` identities per make block, in written order — the
    // lowerer's `inheritedDefaults` consumes these instead of
    // re-resolving from its pre-pass usings (whose import order,
    // aliases, and only/except filtering differ from the checker's).
    std::unordered_map<const ast::MakeDef*, std::vector<std::string>>
        m_resolvedTraits;
    // Each default a trait's body defines, by clause: the trait's identity
    // (registerTraits). Its checked signature takes `This`.
    std::unordered_map<const ast::FunctionClause*, std::string> m_traitDefaultClauses;
    // Each trait's default definitions, by trait identity: a type implementing
    // the trait has them as its own methods (registerMakeSignature).
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>> m_traitDefaults;
    // An inherited default's result, per implementing type, left for the
    // trait's body to answer: that type, and the result's variable.
    std::unordered_multimap<const ast::FunctionClause*, std::pair<TypePtr, int>>
        m_inheritedPendingResults;
    // `type` with each `This` in it the implementing type `receiver`: a trait
    // default as that type has it.
    static auto bindThis(const TypePtr& type, const TypePtr& receiver) -> TypePtr;
    // Whether two registrations are one method: the same parameters, and not
    // the defaults of two different traits (`Geo.Shape`'s and `Ui.Shape`'s
    // `describe`, both `This -> String`).
    auto sameMethod(const Signature& left, const Signature& right) const -> bool;
    std::unordered_map<const ast::Expr*, Signature> m_selectedCallSignatures;
    ShownThrough m_shownThroughDistinct;
    std::unordered_map<const ast::Expr*, DecidedCall> m_decidedCalls;
    std::unordered_set<const ast::Pattern*> m_typeSelectorPatterns;
    std::unordered_map<const ast::Param*, std::string> m_parameterTraits;
    std::unordered_map<const ast::FunctionDef*, std::string> m_makeTargetTraits;
    std::unordered_map<const ast::MakeDef*, std::vector<std::string>> m_makeTargetIdentities;
    // The function each bare call names, as resolved in its scope.
    std::unordered_map<const ast::Expr*, std::string> m_calledFunctions;
    // The module constant each bare name names, as resolved in its scope.
    std::unordered_map<const ast::Expr*, std::string> m_referencedConstants;
    std::unordered_map<const ast::RecordPattern*, std::string> m_recordPatterns;
    std::unordered_map<const ast::TypeExpr*, std::string> m_annotatedRecords;
    // Every declared type (record or not) a written type name means.
    std::unordered_map<const ast::TypeExpr*, std::string> m_annotatedTypes;
    // Each standalone contract (`open : FilePath -> FileMode.Read -> ...`) as
    // annotationToSignature resolved it.
    std::unordered_map<const ast::TypeAnnotation*, Signature> m_contractSignatures;
    // Each `make` target's own methods, across all its blocks (ownMethodKey),
    // and the trait defaults each block inherits for its target: the type's
    // own method of a name and arity beats a default, whichever block it is in.
    std::unordered_map<std::string, std::set<std::string>> m_ownMethodsByTarget;
    std::unordered_map<const ast::MakeDef*, std::vector<InheritedDefault>> m_inheritedDefaults;
    // Every transparent alias the program declares, by name, and what each
    // one a run-time dispatcher tests stands for, as written.
    DeclaredAliases m_declaredAliases;
    std::unordered_map<std::string, std::string> m_structuralAliases;
    // The types modules declare: each by its identity, and the module that
    // declares each name first.
    std::unordered_set<std::string> m_moduleTypeIdentities;
    // Every type name the program declares (kex::collectDeclaredTypeNames).
    std::set<std::string> m_declaredTypeNames;
    // Whether resolving an annotation reports a name it cannot resolve (off
    // while probing a prefix, `FileMode` of `FileMode.Write`), and the
    // written names already reported.
    bool m_reportUnknownTypes = true;
    std::unordered_set<const ast::TypeName*> m_reportedUnknownTypes;
    bool m_declarationsRegistered = false;
    std::vector<std::pair<const ast::TypeName*, SourceLocation>> m_pendingUnknownTypes;
    std::unordered_map<std::string, std::string> m_typeDeclaringModules;
    // The trait each type spelling names in the module it was written in
    // (writtenKey), as resolveTypeExpr resolved it.
    std::unordered_map<std::string, std::string> m_writtenTraits;
    static auto writtenKey(const std::string& scope, const std::string& written)
        -> std::string {
        return scope + "\n" + written;
    }
    // The imported overloads of each binary operator (and of `==` for a
    // `!=`) its scope could see when it was checked: decideOperators runs
    // after the scope (its `using`s) is gone.
    std::unordered_map<const ast::Expr*,
                       std::unordered_map<std::string, std::vector<const ImportedFunction*>>>
        m_operatorImports;
    std::unordered_map<const ast::MethodCall*, StaticTypeAnswer> m_staticTypeOfCalls;
    std::unordered_set<std::string> m_referencedModules;
    // Source module identities declared by the current compilation unit.
    // Local modules take precedence over package interfaces with the same name.
    std::unordered_set<std::string> m_localModules;
    struct ModuleConstructor {
        std::string typeName;
        size_t arity = 0;
        bool isPublic = true;
        // From an imported interface: it is that module's exported function.
        bool imported = false;
    };
    std::unordered_map<std::string,
        std::unordered_map<std::string, ModuleConstructor>>
        m_moduleConstructors;

    // typeName -> constructor names; constructorName -> owning typeName.
    std::unordered_map<std::string, std::vector<std::string>> m_adtVariants;
    std::unordered_map<std::string, std::string> m_adtOfConstructor;
    // Every imported ADT that declares a constructor spelling. The maps above
    // drop a spelling two ADTs share (`Timeout` is Process.CallError's and
    // Net.NetErrorKind's); this keeps them all, for resolution by scope.
    struct ConstructorOwner {
        std::string adt;
        size_t arity = 0;
        bool generic = false;
        // The module declaring `adt`, "" at top level.
        std::string module;
    };
    std::unordered_map<std::string, std::vector<ConstructorOwner>> m_constructorOwners;
    // How many type parameters this program's records and sum types declare,
    // by identity: `Box<A, B>` written for a `record Box<A>` is refused.
    std::unordered_map<std::string, std::size_t> m_declaredTypeArity;
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
        // The same payloads as WRITTEN, and the module that declared them. A
        // sum type is registered before its module's records are, so a
        // payload naming one of them (`RecordDecl(RecordEntry)`) resolved to
        // the bare, unqualified name; a pattern binding re-resolves it here,
        // in the declaring module, once every declaration is known.
        std::vector<const ast::TypeExpr*> payloadExprs;
        std::string modulePath;
        // Declared in this program, so `payloadTypes` lists every payload. An
        // imported constructor's entry carries only its slots.
        bool declaredHere = false;
    };
    std::unordered_map<std::string, ConstructorResult> m_constructorResult;
    // Payload count per ADT constructor (`Just` -> 1, `None` -> 0), used to
    // reject a pattern that destructures the wrong number of values.
    std::unordered_map<std::string, size_t> m_constructorArity;
    std::vector<PatternBinding> m_patternBindings;
    std::unordered_set<const ast::Pattern*> m_stringPatterns;
    // Make-block methods that carry a `:>` declaration. That declaration is
    // authoritative, so the inferred signature of its `let` must not be
    // registered alongside it as a second, permissive overload.
    std::unordered_set<std::string> m_annotatedMethods;
    // Same idea, scoped by RECEIVER as well as name: method name -> the set
    // of receiver types (as typeToString) that have their OWN explicit `:>`
    // contract. m_annotatedMethods alone cannot answer "does THIS type's
    // method have a contract" — only "does ANY type's method by this name"
    // — which let one type's annotation (Char's `string :> String`) block a
    // same-named plain `let` method on an unrelated type (Input's
    // `string(expected: String)`) from ever registering (#251).
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_annotatedReceiverKeys;
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
    auto siblingConstructorJoin(const TypePtr& a, const TypePtr& b) const -> TypePtr;
    // The ADT owning the nullary constructor a NamedType spells, or nullptr.
    // A name that is itself a declared type is that type, never a constructor
    // singleton that happens to share its spelling.
    auto constructorOwner(const NamedType& named) const -> const std::string*;
    // The one ADT a shared constructor spelling means from here: declared by
    // an enclosing module, an imported one, or an automatically imported
    // one. Nullptr when none or several are visible.
    auto scopedConstructorOwner(const std::string& name) const
        -> const ConstructorOwner*;
    // `scope`: a module the call names (`Model.RecordDecl(entry)`), looked in
    // first; otherwise the enclosing modules, innermost first.
    // The constructor `name` resolves to there, by the same rules.
    auto constructorInfo(const std::string& name, const std::string& scope = {}) const
        -> const ConstructorResult*;
    auto constructorResultType(const std::string& name,
                               const std::vector<TypePtr>& argTypes,
                               const std::string& scope = {})
        -> TypePtr;
    // Payload `i` of `info` as declared, resolved again in the declaring
    // module now that its records and aliases are known: an ADT is
    // registered before them.
    auto declaredPayload(const ConstructorResult& info, std::size_t i) -> TypePtr;
    // Errors when a constructor is applied to the wrong number of payloads
    // (`Broken(5)` for a nullary `Broken`).
    // The first argument of a call to `sig` that disagrees with what earlier
    // arguments bound a shared generic to, and the type it had to be.
    auto genericConflict(const Signature& sig, const std::vector<TypePtr>& argTypes,
                         const std::function<TypePtr(std::size_t, const TypePtr&)>& selectedType)
        -> std::optional<std::pair<std::size_t, TypePtr>>;
    // Errors when an integer literal where a Byte goes is out of its range.
    auto checkByteLiterals(const ast::Expr& value, const TypePtr& expected) -> void;
    auto checkConstructorPayloads(const std::string& name,
                                  const std::vector<TypePtr>& argTypes,
                                  SourceLocation loc, const std::string& scope = {}) -> void;
    // Errors when a constructor pattern names an arm of a DIFFERENT ADT than
    // the value being matched — `if let Ok(x) = anOptional`.
    auto checkPatternConstructorOwner(const ast::Pattern& pattern,
                                      const TypePtr& expected) -> void;
    std::unordered_set<std::string> m_nullaryConstructors;
    // Each ADT's nullary variants, by the ADT's identity: what a variant type
    // (`FileMode.Write`, NamedType::variant) may name.
    std::unordered_map<std::string, std::unordered_set<std::string>> m_nullaryVariants;
    auto isNullaryVariant(const std::string& adt, const std::string& name) const -> bool {
        const auto variants = m_nullaryVariants.find(adt);
        return variants != m_nullaryVariants.end() && variants->second.contains(name);
    }

    // Record field types: typeName -> { fieldName -> TypePtr }.
    // Populated by a pre-pass over all RecordDefs so field access in method
    // chains (`user.name`, `point.x`) returns the declared field type.
    std::unordered_map<std::string,
                       std::unordered_map<std::string, TypePtr>> m_recordFields;
    // The records the checked program itself declares, as opposed to the
    // imported ones seeded into m_recordFields from interfaces.
    std::unordered_set<std::string> m_programRecords;
    // This program's distinct types, by name and by qualified name.
    std::unordered_set<std::string> m_programDistinctTypes;
    // Type variables standing for unannotated FUNCTION PARAMETERS. The
    // field-read rule of kexhq/kex#242 infers a record for these and nothing
    // else: a variable bound by a pattern over an imported constructor
    // (`MainDef(info)`) is untyped only because interfaces carry no payload
    // types, and guessing a record for it misread `info.body`.
    std::unordered_set<int> m_unannotatedParamVars;
    // Fields without a default whose declared type is not Optional.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_requiredRecordFields;
    auto registerRecordFields(const ast::Program& program,
                              bool namesOnly = false) -> void;
    // A dotted type name's nominal identity — the record or ADT it spells,
    // through an alias or a `using`-relative path if need be — or nullopt
    // when it names no declared type.
    auto qualifiedNominalType(const std::string& qualified) const
        -> std::optional<std::string>;
    auto resolveRecordName(const std::string& name) const -> std::string;
    // Whether the built-in `op` cannot apply to `type`: a record or ADT,
    // declared here or imported, a tuple, map or open record, a list for
    // anything but `+`, a distinct type over one of those, or a value typed
    // only by a non-structural trait. Such a type has only the operators
    // declared for it.
    auto lacksBuiltinOperator(const TypePtr& type, const std::string& op) const
        -> bool;
    // Whether such a type gets `op` from a trait: one it implements, or the
    // trait it is typed as, requiring or defaulting it.
    auto traitDeclaresOperator(const TypePtr& type, const std::string& op) const
        -> bool;
    // Whether anything declares `op` for `type`: its own make block, an
    // imported one, or a trait.
    auto declaresOperatorFor(const TypePtr& type, const std::string& op) const
        -> bool;
    // An overload of an operator: its signature, and the imported function
    // that implements it (null for one of this unit's own make blocks).
    struct OperatorOverload {
        Signature signature;
        const ImportedFunction* imported = nullptr;
    };
    // The overloads of `op` the type of a left operand declares itself: a
    // make block for it, local or imported, not a generic one.
    auto operatorOverloadsFor(const TypePtr& left, const std::string& op,
                              const std::vector<const ImportedFunction*>& imported)
        -> std::vector<OperatorOverload>;
    // Decides every binary operator of `program` once its operands are typed
    // (m_decidedCalls): the left type's own overload (Operator, naming the
    // implementation and, for an imported one, its backend function), dispatch
    // on the value at run time (Dispatch, when the left type is not known), or
    // the built-in operator (Builtin, with what its operands are). The one
    // place an operator is decided; lowering and the walker read it.
    auto decideOperators(const ast::Program& program) -> void;
    // Whether a value of `type` is of one type the check can name, rather than
    // whichever type it turns out to be when it runs.
    auto knownWhenChecked(const TypePtr& type) const -> bool;
    // Resolves a bare module segment written at a call site (`Set.from(…)`)
    // against the modules `using` brought into scope, when the literal name
    // is not itself a registered module. See the .cxx for the precedence
    // rules (kexhq/kex#229).
    auto resolveModulePath(const std::string& name,
                          const std::string& member) const -> std::string;
    // The same, without the enclosing module's own nested modules.
    auto resolveImportedModulePath(const std::string& name,
                                   const std::string& member) const -> std::string;

    // Type alias map — populated before function bodies are checked.
    std::unordered_map<std::string, TypePtr> m_typeAliases;
    std::unordered_map<std::string, ImportedDistinctType> m_distinctTypes;

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
    // Local variable name -> the plain top-level function it was bound from
    // via a bare `~name` capture (`let f = ~greet`). A curried reference's
    // type is just a FuncType — positional parameter TYPES, no names — so
    // without this, a named-argument call through such a local (`f(loud:
    // true, name: "Ada")`) has no param names to match against and falls
    // back to matching by the call site's written order, which silently
    // swaps same-typed arguments (kexhq/kex#309). Best-effort: keyed by bare
    // variable name, so a shadowing rebind in a nested scope can leave a
    // stale entry for an outer same-named binding; harmless since the
    // lookup is only consulted after confirming the CURRENT binding really
    // is a FuncType.
    std::unordered_map<std::string, std::string> m_curryOrigin;
    // Receiver-aware signatures declared inside `make T do` blocks. Their
    // first parameter is the implicit receiver, matching UFCS call checking.
    std::unordered_map<std::string, std::vector<Signature>> m_methodSignatures;
    // Protocol entry names declared with `slot`, used to recognize the
    // compiler-owned `within:` call option on a Server<X> receiver.
    std::unordered_set<std::string> m_slotMethodNames;
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
    // Where the function clause being checked collects the `.try`s of its own
    // body, and the block depth that body is at.
    std::vector<std::pair<SourceLocation, TypePtr>>* m_clauseTries = nullptr;
    int m_clauseTriesDepth = 0;
    // The `return`s of the clause being checked (not a block's), each with its
    // value's type, for the check against the declared result.
    std::vector<std::pair<const ast::Expr*, TypePtr>>* m_clauseReturns = nullptr;
    // The functions with no declared result whose `.try` can fail: each
    // answers its successful value wrapped, `Just` or `Ok` (successWrappers).
    std::unordered_map<const ast::Expr*, std::string> m_successWrappers;
    int m_tryBelowBlock = 0;
    // The complete receiver type of the current `make X do` block, used for
    // `this`, `This`, and receiver-aware body inference.
    TypePtr m_currentMakeType;
    // The trait whose default bodies are being checked: what `this` there
    // must answer to.
    const TraitDef* m_currentTrait = nullptr;
    // The make target's type variables by name (the `D` of `make Qty<D>`), for the
    // length of the block: its methods' annotations name the same `D`.
    std::unordered_map<std::string, TypePtr> m_currentMakeTargetVars;
    // Module owning the `make` block being checked, "" at top level.
    std::string m_currentMakeModule;
    bool m_inCastSlot = false;

    // Populated by inferExpr; maps each visited Expr node to its inferred type.
    std::unordered_map<const ast::Expr*, TypePtr> m_typeMap;
    std::unordered_map<const ast::Expr*, TypePtr> m_assignmentTargetTypes;

    // Unification substitution: TypeVar id → the concrete/constrained type it
    // was unified with during body inference. Never cleared between functions
    // because TypeVar IDs are globally unique (monotonically incrementing).
    std::unordered_map<int, TypePtr> m_subst;
    // While a function's body is checked: the type variables its signature
    // writes, by id, with the names written. They stand for every type, so
    // the body may not use them as one (kexhq/kex#644).
    std::unordered_map<int, std::string> m_rigidVars;
    // While set, every binding unifyVar adds is noted here, so a probe
    // (memberCompletions) can take its bindings back.
    std::vector<int>* m_substJournal = nullptr;

    // The result variable the pre-pass gives a make-block method with no
    // return annotation, per clause, until that clause's body is checked.
    // Only the body may bind it: a call checked first used to, and pinned it
    // to whatever it met — `IO.printLine(a + b)` made `+` answer `Showable`.
    std::unordered_map<const ast::FunctionClause*, int> m_pendingResults;
    std::unordered_set<int> m_pendingResultVars;
    // Checks a use made of a pending result before the body decided it, run
    // with what it decided.
    std::unordered_map<int, std::vector<std::function<void(const TypePtr&)>>>
        m_pendingChecks;

    // Walk m_subst to find the concrete type behind a TypeVar, following any
    // chain of substitutions. Returns `t` unchanged for non-TypeVar types.
    auto resolve(TypePtr t) const -> TypePtr;

    // Record that TypeVar `id` equals `concrete`.
    // No-op if `id` already has a binding (first writer wins).
    auto unifyVar(int id, TypePtr concrete) -> void;
    auto fixNestedVariables(const TypePtr& argument, const TypePtr& parameter) -> void;
    auto resolvedThroughout(const TypePtr& type) const -> TypePtr;

    auto freshTypeVar() -> TypePtr;

    // Let-polymorphism. Rewrites the inference variables a definition left
    // free — those created while checking it, hence `varMark` — into the
    // per-signature generic placeholders (negative ids) that call sites
    // already instantiate afresh. See the definition for why this is the
    // whole of the fix.
    auto generalizeSignature(Signature& signature, int varMark) -> void;
    // The make target's own variables (the `D` of `make Qty<D>`: the free
    // variables of `receiver`) in a signature, as per-call generics: shared
    // across the block, the first call bound them for every other. Only
    // those: a plain `let`'s unannotated parameters and pending result stay
    // the variables the body answers.
    auto generalizeTargetVariables(Signature& signature, const TypePtr& receiver) -> void;
};

} // namespace kex::semantic

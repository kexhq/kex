#pragma once

#include "lower.hxx"
#include "lower_internal.hxx"
#include "../hir/hir.hxx"
#include "../ast/function_groups.hxx"
#include "../ast/type_text.hxx"
#include "../common/mangle.hxx"
#include "../common/well_known.hxx"
#include "../semantic/declared_type.hxx"
#include "../common/utf8.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/signature_params.hxx"
#include "../common/named_arguments.hxx"
#include "../common/source_name.hxx"
#include "../lexer/token.hxx"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace kex::ir {

using namespace kex::ir::detail;

using ast::methodDispatchTypes;
using ast::typeText;
using ast::TypeSpelling;
using ast::separableOverloadSignatures;

struct Lowering {
    int counter = 0;
    // `__loopN` -> the Kex name of its `loop do |i|` iteration counter, for the
    // loop functions that bind one. The counter rides along as a loop-carried
    // parameter, but its tail-call argument is bumped instead of threaded.
    std::unordered_map<std::string, std::string> loopCounters;
    std::string sourceFile;
    std::string outputModule;
    // Exact externally-callable protocol surface for each serving state.
    // Ordinary helpers share the compiled module but must never become OTP
    // request handlers merely because erlang:apply/3 can see them.
    std::unordered_map<std::string, std::vector<std::string>> servingSlotsByType;
    // Slots declared to return Void: asynchronous casts, the rule the
    // checker's `isServingCast` applies.
    std::unordered_set<std::string> servingCastSlots;
    // Serving types whose block declares an `upgrade` method — the hook the
    // serving runtime runs on a live state after a hot reload.
    std::set<std::string> servingUpgradeTypes;
    const SourceLocation* currentLoc = nullptr;
    // Names bound with `let` (immutable) — a mutating `!` call on one is a
    // runtime error matching the walker's behaviour.
    std::unordered_set<std::string> immutableBindings;
    // Record layout, by record name: field names in declared order (tuple
    // position = index + 2, since element 1 is the 'Name' tag) and their
    // default-value exprs (nullptr = no default). Drives construction (fields
    // in declared order, defaults filled) and field access.
    struct RecordInfo {
        std::vector<std::string> fields;
        std::vector<const ast::ExprPtr*> defaults;
        // The module that DECLARED the record, and the names it had in scope.
        // A default is written against those, not against whatever is in
        // scope wherever the record happens to be constructed.
        std::string owner;
        std::unordered_map<std::string, std::string> imports;
        std::unordered_map<std::string, std::string> aliases;
        // A record of a unit compiled apart: the BEAM module whose
        // `__kex_record_default` answers its defaults, and which fields have one.
        std::string defaultsModule;
        std::vector<bool> defaultedApart;
    };
    std::unordered_map<std::string, RecordInfo> records;
    // Records that came in from a separately compiled module (the prelude's)
    // under their bare name, as opposed to ones this unit declares.
    std::unordered_set<std::string> externalRecordNames;
    // Records this unit declares, by the name `records` keys them under.
    std::unordered_set<std::string> localRecordNames;
    // Names this unit declares a record under that an interface lays out
    // differently (collectRecord).
    std::unordered_set<std::string> conflictingRecordLayouts;
    // A record or ADT of a unit compiled apart, by name: the module holding
    // its `make` functions (ExternalRecordLayout::methodOwner).
    std::unordered_map<std::string, std::string> externalMethodOwners;
    // Set while a receiver guard is built for a `make` target, which is
    // written against the block's own module rather than the use site's.
    mutable bool resolvingMakeOwner = false;
    // What `Type.fields` and `Type.constructors` answer with at runtime: each
    // record's fields and each sum type's variants as declared, in order,
    // with their declared types. Ordered maps keep the emitted Core
    // reproducible.
    struct FieldLayout {
        std::string name;
        semantic::StructuredType type;
        bool hasDefault = false;
    };
    std::map<std::string, std::vector<FieldLayout>> recordLayouts;
    std::map<std::string,
             std::vector<std::pair<std::string, std::vector<semantic::StructuredType>>>>
        adtLayouts;

    auto moduleLocalTypeSpelling(const std::string& written) const
        -> std::string;

    auto traitIdentityOf(const std::string& written, const std::string& scope,
                         bool lastResort = false,
                         const std::set<std::string>* imports = nullptr) const
        -> std::string;
    auto traitIdentityOf(const std::string& written) const -> std::string;

    auto canonicalRecordName(const std::string& name) const -> std::string;
    // field name → the record slots holding it. A field can live in several
    // records at (possibly different) positions; the emitted accessor
    // dispatches on the tag when they differ. Mirrors the string emitter.
    //
    // `tupleSize` is the record's Core Erlang tuple size (fields + tag), which
    // is what tells two layouts of the SAME tag apart — a local `record
    // Request` next to the prelude's. Tag alone cannot: reading the local
    // 2-element layout's position out of the prelude's 6-element Request
    // returned the wrong field silently on BEAM, where the walker read both
    // correctly.
    struct FieldSlot {
        std::string record;
        int position;
        int tupleSize;
    };
    std::unordered_map<std::string, std::vector<FieldSlot>> fieldAccessors;
    // Names that resolve to a local function (top-level fn, make-block method,
    // or record field accessor). Only these may use the "UFCS → local apply,
    // receiver-first" fallback; any other `.method` is an unported builtin and
    // must error, not silently become a call to a nonexistent function.
    std::unordered_set<std::string> localMethods;
    // "name/arity" for every locally defined method, including the receiver.
    // A UFCS call may only take the local path when a local definition exists
    // at that arity — otherwise it emits `apply 'get'/3` for a function that
    // was never defined (the make block only had get/2), which erlc rejects.
    std::unordered_set<std::string> localMethodArities;
    // Field names whose `name/1` in this module is the SYNTHESIZED record
    // accessor rather than a function someone wrote. Such a call never takes
    // a capability context, whatever else the name means at another arity.
    std::unordered_set<std::string> accessorFields;
    // Every make-block method by name, with the type its block targets and the
    // module that block is written in. A receiver call that labels arguments
    // or leaves defaulted ones out places them against the method it binds.
    struct MakeMethod {
        std::string owner;
        std::string modulePath;
        const ast::FunctionDef* def;
    };
    std::unordered_map<std::string, std::vector<MakeMethod>> makeMethods;
    // Cross-type method-name collisions: a method name → the make-block type
    // names that define it, in order. When more than one type defines the
    // same name, each type's version is emitted mangled (`name/Type`) and a
    // dispatcher under the bare name selects at runtime on the receiver's tag
    // (element 1). Mirrors the string emitter's collision handling.
    std::unordered_map<std::string, std::vector<std::string>> methodOwners;
    std::unordered_set<std::string> collidingMethods;
    // Prefix calls and receiver dispatch have distinct symbols even when
    // their source name and arity coincide.
    std::unordered_map<std::string, std::string> freeFunctionSymbols;

    auto prefixSymbol(const std::string& name) const -> std::string;

    auto prefixCall(const std::string& name, std::vector<ExprPtr> args) -> Call;
    // Same receiver + name + BEAM arity, but distinct checked parameter
    // signatures. These implementations need their full signature in the
    // local symbol rather than the receiver-only collision spelling.
    std::unordered_set<std::string> argumentOverloadedMethods;
    // Local overload key -> (emitted exact symbol, receiver-first dispatch
    // types). Operators use this to build a guarded bare-symbol dispatcher;
    // named calls select the exact symbol directly from semantic analysis.
    std::unordered_map<std::string,
        std::vector<std::pair<std::string, std::vector<std::string>>>>
        argumentOverloadSignatures;
    // Operator symbols overloaded by a make block (`make T do let +(o) ...`).
    // The corresponding binary-op Intrinsic then dispatches at runtime: a
    // tuple (record) receiver → the user's `'+'/2`, otherwise the builtin.
    std::unordered_set<std::string> overloadedOps;
    // Make-block methods emitted WITH an implicit `this` receiver. A static
    // `Type.method(args)` call to one must pass a placeholder receiver so the
    // arities line up (the method builds its own value from the args and never
    // touches `this` — see examples/json_parser.kex's `Parser.parse(input)`).
    std::unordered_set<std::string> implicitThisMethods;
    // `type Handler = Env -> String` names a SHAPE, not a tag: no value ever
    // carries 'Handler', so a dispatch guard has to test what the alias
    // stands for. Alias name -> the rendered dispatch type of its single
    // right-hand side. Only structural right-hand sides are recorded — a
    // single NAMED variant is an ADT tag (or the existing transparent alias),
    // which `variantTagSet` and `records` already speak for.
    std::unordered_map<std::string, std::string> structuralTypeAliases;
    // `distinct type Tuplex = (Number, Number)`: erased at run time, so no
    // dispatch guard can tell a Tuplex from any other pair. An operator on
    // one is called by its static type instead.
    std::unordered_set<std::string> distinctTypes;
    // What each transparent alias stands for, for the field and constructor
    // layouts `Type.fields` and `Type.constructors` answer (kexhq/kex#530).
    semantic::DeclaredAliases declaredAliases;
    // Record/make type names (so a namespace call can be recognized as a
    // static method dispatch, not an unknown module).
    std::unordered_set<std::string> knownTypes;
    // Top-level `let name = expr` bindings become 0-arity functions; a bare
    // reference to one compiles to `apply 'name'/0()`, not a variable.
    std::unordered_set<std::string> topLevelConstants;
    // Top-level zero-parameter functions (e.g. `foul name do ... end`). A bare
    // reference to one (no parens) is a call `apply 'name'/0()`, not a var.
    std::unordered_set<std::string> zeroArgFns;
    // `Module.name` for every parameterless module member. A bare reference
    // from inside that module is a call to it — the only way to reach one
    // that `private do` keeps out of the module's exports.
    std::unordered_set<std::string> moduleZeroArgFns;
    // The EMITTED (mangled) symbols of those same members, so an import site
    // holding only the emitted name can tell a zero-arg constant from an
    // ordinary function without unmangling. `using Limits` + a bare `LIMIT`
    // is a call to the constant, not a nullary ADT constructor (kexhq/kex#282).
    std::unordered_set<std::string> emittedModuleZeroArg;
    // Capability substitution threads one context — a map from capability
    // name to implementation — through foul functions instead of keeping
    // per-process state (kexhq/kex#181). Nothing is emitted unless the unit
    // declares a capability, so a program using none compiles unchanged.
    std::unordered_set<std::string> foulFns;
    // "name/arity" for every NON-foul definition in this unit. `foulFns` is
    // keyed by name alone, so a foul function anywhere in the file makes every
    // same-named call take a context — including calls to a pure function of
    // that name at a different arity, in a different module of the same file.
    // Net.HTTP is the case: `make Client do foul request(…)` put "request" in
    // foulFns, and `module HTTP`'s pure `request/4` was then called as
    // `request/5`, which erlc rejects. A definition that IS pure at exactly
    // the arity being called settles it.
    std::unordered_set<std::string> pureFnArities;
    // The program's own top-level foul functions, as `name/arity`. A module's
    // public pure function records its BARE name in `pureFnArities` too (for
    // calls that reach it through a `using`), and that entry used to win over
    // the program's own foul function of the same name and arity: the stdlib's
    // `Random.seeded/1` made a top-level `foul seeded(store)` go out as
    // `seeded/1` against a defined `seeded/2` (kexhq/kex#494).
    std::unordered_set<std::string> topLevelFoulArities;
    std::unordered_set<std::string> capabilityModules;
    // Each capability declared in this unit and the foul members it declares.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        capabilityMembers;
    // Name of the context variable in scope; empty means there is none, and
    // a call from there passes a fresh empty map.
    std::string currentContextVar;
    auto threadsCapabilities() const -> bool;
    // Top-level free function → its ordered parameter names ("" for an
    // unnamed/pattern param). Lets a call with named args reorder them into
    // positional slots.
    std::unordered_map<std::string, std::vector<std::string>> fnParamNames;
    std::unordered_map<std::string, std::vector<const ast::FunctionClause*>> fnNamedClauses;
    // Every parameter name ANY overload of that function declares. fnParamNames
    // keeps one clause set, which is all the positional reordering needs but
    // not enough to judge a label unknown: `render(text:)` is valid for the
    // 1-param overload even when the 2-param one was recorded.
    std::unordered_map<std::string, std::set<std::string>> fnAllParamNames;
    // Top-level trailing defaults, used to expand omitted arguments at local
    // call sites.
    std::unordered_map<std::string, std::vector<const ast::ExprPtr*>> fnDefaults;
    std::unordered_map<std::string,
        std::vector<std::pair<std::size_t, std::string>>> fnTraitParams;
    // Names that are real functions in this module (top-level + make methods).
    // A call to a name NOT in here is an indirect apply through a variable
    // holding a fun (e.g. a `block` parameter).
    std::unordered_set<std::string> knownFns;
    // "name/arity" for every function DEFINED in this unit, at the arity it is
    // emitted with — a make method's counts its receiver. `knownFns` is
    // name-only, so it cannot tell a bare call that matches no definition's
    // arity from one that does.
    std::unordered_set<std::string> definedFnArities;
    // Qualified Kex module function (`Util.Math.double`) -> local BEAM name.
    // Modules in a single source file still flatten into one BEAM module, so
    // this preserves qualification without a cross-module call.
    std::unordered_map<std::string, std::string> moduleFunctions;
    // Receiver type -> source module path for make blocks compiled into a
    // companion module in this same unit.
    std::unordered_map<std::string, std::string> localTypeModules;
    // Type name -> the module that DECLARES it, for every type written inside
    // a module, `make` target or not. An opaque `type TCPListener` declared in
    // Net.Socket.TCP carries its QUALIFIED name as its runtime tag, the same
    // rule records follow, so a guard built from the written `TCPListener`
    // matched nothing. Separate from `localTypeModules`, which is keyed by
    // make target and drives call routing rather than tag identity.
    std::unordered_map<std::string, std::string> moduleDeclaredTypes;
    // The same declarations by their qualified identity. The map above keeps
    // one module per bare name, which cannot say whether THIS module declares
    // a `Connection` once two of them do.
    std::unordered_set<std::string> declaredTypeIdentities;
    // Bare names of module-scoped free functions acting as receiver methods
    // for a type their own module declares (see preModuleFn).
    std::unordered_set<std::string> moduleReceiverNames;
    // Current module path while lowering a module body. Nested module-relative
    // qualified calls (`Router.get` inside `module Http`) resolve against it.
    std::string currentModulePath;
    // The emitted symbol of the function being lowered right now. A module
    // constant must not resolve to ITSELF from inside its own body: `let
    // Monday = Monday` in `module Days` aliases the OUTER `Monday`, and
    // resolving the right-hand side to `Days.Monday/0` makes it read itself
    // forever (spec/module_self_alias.kex).
    std::string currentFunctionSymbol;
    // Declared types of the parameters in scope, by source name — the
    // annotation a call site cannot see. Saved and restored per clause.
    std::unordered_map<std::string, std::string> declaredParamTypes;
    // Receiver type of the make block currently being lowered. Record field
    // names are not globally unique, so `this.field` must select the layout
    // owned by this type rather than the first same-named field in the file.
    std::string currentMakeType;
    // Modules a `using` has brought into scope at this point. The block form
    // saves and restores it around its body, exactly as it does moduleImports.
    std::set<std::string> usingModules;
    // Bare imported function name → mangled name. Populated by `using M, only:`
    // inside module bodies so bare calls resolve to the correct cross-module fn.
    std::unordered_map<std::string, std::string> moduleImports;
    // Bare name → `Module.name` for what a `using` imported from a separately
    // compiled module (see importExternalModule). Scoped like moduleImports.
    std::unordered_map<std::string, std::string> externalImports;
    // Lexically visible `using Source, as: Alias` mappings. The receiver's
    // first path segment is expanded before qualified module lookup.
    std::unordered_map<std::string, std::string> moduleAliases;
    // Loaded external modules from KexI registry (/load).
    const ExternalModules* externalModules = nullptr;
    // Lowering the prelude itself: its `to` conversions are what other
    // units' fall back to, so they forward among themselves instead.
    bool lowersPrelude = false;
    // Exact imported call ownership selected by semantic analysis.
    const std::unordered_map<const ast::MethodCall*,
        semantic::ResolvedCallTarget>* resolvedCalls = nullptr;
    // The checked program (src/hir/): what each call is, decided once by
    // semantic analysis. Null when lowering an unchecked program.
    const hir::Program* hir = nullptr;
    // The type whose overload `n` applies, as the checker decided it; "" for a
    // built-in operator. Lowering used to read it off the left operand's
    // recorded type; with that disabled, spec-beam, fuzz and the examples
    // were unchanged.
    auto operatorOwner(const ast::BinaryOp& n) const -> std::string;
    // The checker's static type for `expr` (null when it has none).
    auto staticTypeOf(const ast::Expr* expr) const -> semantic::TypePtr;
    // `implement:` identities per make block, in written order, resolved by
    // semantic analysis. Absent without analysis (`--no-check`), in which
    // case `inheritedDefaults` resolves from its pre-pass usings as before.
    const std::unordered_map<const ast::MakeDef*, std::vector<std::string>>*
        resolvedTraits = nullptr;
    const StaticTypeOfCalls* staticTypeOfCalls = nullptr;
    const ExpressionTypes* expressionTypes = nullptr;
    // Lexical source binding -> trait -> hidden dictionary IR binding.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> traitDictionaries;
    std::unordered_map<std::string,
        std::vector<ExternalModules::TraitMethod>> traitMethods;
    // ADT/variant type → its tag names (e.g. Optional → {"Just","None"}). Used
    // by the dispatcher to wildcard-match any variant of a type, not just the
    // type name itself (which isn't set as element(1) on any variant value).
    std::unordered_map<std::string, std::vector<std::string>> typeVariantTags;
    // Nullary variant tags (no payload) — dispatched as atoms, not tuples.
    std::unordered_set<std::string> nullaryVariantTags;
    // Every variant tag of every ADT (flat).
    std::unordered_set<std::string> variantTagSet;
    auto qualifiedVariantTag(const std::string& name) const -> std::string;
    auto structuredTypeLiteral(const semantic::StructuredType& type) -> ExprPtr;

    auto runtimeError(const std::string& msg) -> ExprPtr;

    auto capabilityDispatchLowered(
        const std::string& capability, const std::string& method,
        std::vector<ExprPtr> loweredArgs, std::vector<Binding>& binds,
        const std::function<ExprPtr(std::vector<ExprPtr>)>& directCall)
        -> ExprPtr;

    auto isCapabilityModule(const std::string& name) const -> bool;
    auto checkedCallModule(const ast::MethodCall& n) const -> std::string;

    auto capabilityInterfaceHas(const std::string& capability,
                                const std::string& method) const -> bool;

    auto currentContext() -> ExprPtr;
    auto callNeedsContext(const std::string& name, int arity) const -> bool;
    auto externalConstant(const std::string& name) -> ExprPtr;

    auto externalCallExpr(const std::string& qualKey, const std::string& atom,
                          const std::string& beamFunction, int arity,
                          std::vector<ExprPtr> args) -> ExprPtr;
    auto localCallExpr(const std::string& name, std::vector<ExprPtr> args)
        -> ExprPtr;
    auto ownsMethod(const std::string& typeName,
                    const std::string& method) const -> bool;

    auto localCall(const std::string& name, std::vector<ExprPtr> args)
        -> Call;

    auto undefinedMethod(const std::string& method, ExprPtr receiver,
                         const std::string& hint = {}) -> ExprPtr;

    auto undefinedMethodOrField(const ast::MethodCall& n, ExprPtr receiver)
        -> ExprPtr;

    auto unknownNamedArgument(const std::string& qualified,
                              const std::string& label) -> ExprPtr;

    static auto opSymbol(TokenType t) -> std::string;

    static auto simpleTypeName(const ast::TypeExprPtr& t) -> std::string;
    static auto makeTargetNameList(const ast::TypeExprPtr& t,
                                   const std::string& module = "",
                                   const std::set<std::string>& records = {})
        -> std::vector<std::string>;
    // SSA renaming: Kex source name → its current IR variable name. A `let`/
    // `var`/reassignment introduces a fresh name and updates this; Identifier
    // lowering consults it. Function params map to themselves (the emitter
    // uppercases). This is the SSA construction the string emitter did via
    // m_varSubst, now done once in the lowering pass.
    std::unordered_map<std::string, std::string> subst;
    // A dynamic field read evaluates its receiver once, then shares that
    // value with the ordinary UFCS fallback without changing AST identities
    // used by the checker's resolved-call and expression-type tables.
    std::unordered_map<const ast::Expr*, std::string> loweredReceivers;
    // Local (Kex source) name -> the plain top-level function it was bound
    // from via a bare `~name` capture (`let f = ~greet`), so a named-arg
    // call through `f` can reorder by `greet`'s real parameter names instead
    // of the call site's written order (kexhq/kex#309). Best-effort: not
    // snapshotted alongside `subst` at branch points, so a stale entry can
    // leak across sibling branches — harmless, since it is only consulted
    // for a name that is ALSO still a lexical local at the call site.
    std::unordered_map<std::string, std::string> curryOrigin;

    auto fresh(const std::string& hint = "T") -> std::string;

    auto currentName(const std::string& kexName) -> std::string;

    // Clone an ATOMIC expr (Var/Lit) — used when an operand must appear in
    // several positions (e.g. operator-overload dispatch). Only atomic nodes
    // are ever cloned, so this stays trivial.
    // Produce a fresh copy of an atomic (Var/Lit) expression. All call sites
    // pass a result of atomize/atomize_ir, which is guaranteed to be Var or Lit.
    // To work around a GCC-specific issue where a unique_ptr argument appears
    // null due to indeterminate function-parameter initialization ordering, we
    // store the atomic identity (name or lit value) eagerly and reproduce it on
    // demand rather than reading through the pointer at clone-time.
    struct AtomicRef {
        std::string name;
        std::optional<Lit> lit;
        auto get() const -> ExprPtr {
            if (!name.empty()) return var(name);
            if (lit) { auto e = std::make_unique<Expr>(); e->node = *lit; return e; }
            return var("_clone_bug");
        }
    };
    auto snap(const ExprPtr& e) -> AtomicRef;

    auto clone(const ExprPtr& e, const char* site = "?") -> ExprPtr;

    auto wrapLets(std::vector<Binding>& binds, ExprPtr body) -> ExprPtr;

    static auto isAtomic(const Expr& e) -> bool;

    auto atomize(const ast::ExprPtr& e, std::vector<Binding>& binds) -> ExprPtr;


    auto opOf(TokenType t) -> Op;

    static auto isOrderingOp(Op op) -> bool;

    static auto orderingFunction(Op op) -> std::string;

    auto isIntegerOperand(const ast::ExprPtr& e) const -> bool;

    auto comparableKind(const ast::ExprPtr& e) const -> std::string;

    auto exactEqualityType(const ast::ExprPtr& e) const -> std::string;

    auto moduleConstantSymbol(const std::string& name) const -> std::string;

    auto lowerFieldDefault(const RecordInfo& info, const ast::ExprPtr& e)
        -> ExprPtr;

    // ---- Expression lowering ---------------------------------------------
    auto lower(const ast::ExprPtr& e) -> ExprPtr;

    static auto curryOp(const std::string& name) -> std::optional<Op>;
    auto lowerNameAsUfcsFun(const std::string& name,
                            const std::vector<ast::ExprPtr>& args) -> ExprPtr;

    auto lowerCurry(const ast::CurryExpr& n) -> ExprPtr;


    auto clauseNames(const ast::FunctionClause* clause,
                     const std::vector<std::string>& fallback) -> std::vector<std::string>;

    auto blockFitsExactOverload(const std::string& key, size_t count) const
        -> bool;

    auto fillOmittedArguments(const std::vector<std::string>& defaults,
                              std::vector<ExprPtr>& slots) -> void;

    auto fillDefaultSlots(const std::string& name,
                          std::vector<ExprPtr>& slots,
                          std::vector<Binding>& binds,
                          const ast::FunctionClause* clause = nullptr) -> void;

    auto importedKey(const std::string& name) const -> std::string;

    auto nextExtendingModule(const std::string& method,
                             const std::string& typeName, int arity) const
        -> std::string;

    auto makeMethodFor(const ast::MethodCall& n) const -> const MakeMethod*;

    auto staticFieldRead(const ast::MethodCall& n) -> ExprPtr;

    auto typeOwnsStaticFunction(const std::string& type,
                              const std::string& method) const -> bool;

    auto selectedMakeMethod(const ast::MethodCall& n) const -> const MakeMethod*;
    auto lowerResolvedCall(const ast::MethodCall& n, const semantic::ResolvedCallTarget& target,
                           const std::string& capabilityTarget) -> ExprPtr;
    auto lowerServerSlotCall(const ast::MethodCall& n, const semantic::ResolvedCallTarget& target)
        -> ExprPtr;
    auto lowerValueReceiverCall(const ast::MethodCall& n) -> ExprPtr;



    auto receiverOwner(const ast::MethodCall& n) const -> std::string;
    // The receiver type's own implementation of a call, where the run-time
    // dispatch cannot reach it (see the definition): which reason applied,
    // and the symbol to call.
    struct OwnImplementation {
        enum class Reason { None, DistinctType, ModuleFunction };
        Reason reason = Reason::None;
        std::string symbol;
    };
    auto ownImplementationOf(const ast::MethodCall& n) const -> OwnImplementation;
    static auto ownerNameOf(const std::string& printed) -> std::string;
    // The name `owner`'s implementation of `method` is emitted under when
    // several types define it (`count/List`), or nullopt when it keeps the
    // bare name or an argument-overloaded one. Definitions and direct calls
    // both name it through this, so the two cannot disagree.
    auto collidingImplementationName(const std::string& method,
                                     const std::string& owner,
                                     size_t sourceArity) const
        -> std::optional<std::string>;
    // Whether every clause of `method` binds its parameters by plain name —
    // none matches on an argument (`to(String)`), so calling its
    // implementation directly means the same as going through the dispatcher.
    auto takesPlainParameters(const std::string& method,
                              const std::string& owner) const -> bool;

    auto placeMethodArguments(const ast::MethodCall& n, const MakeMethod& method,
                              std::vector<Binding>& binds) -> std::vector<ExprPtr>;

    auto lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr;

    // Receiver-call and stdlib compatibility resolution. Unknown forms fail
    // explicitly rather than silently emitting a nonexistent function.
    // Set while lowering a mutating `!` call's VALUE (the rebind itself is
    // handled by the enclosing statement — see lowerBodyFrom/loop handling).
    bool m_lowerMutatingAsValue = false;

    auto localTypeModule(const std::string& type) const
        -> std::unordered_map<std::string, std::string>::const_iterator;

    auto makeTraitDictionary(const std::string& trait,
                             const ast::Expr& value) -> ExprPtr;

    auto makeTraitDictionaryFor(const std::string& trait,
                                const std::string& concreteType) -> ExprPtr;

    auto makeDictionaryWrapper(const FunDef& def) -> std::optional<FunDef>;

    static auto modulePath(const ast::Expr& e, std::vector<std::string>& out) -> bool;

    static auto beamModulePath(const ast::Expr& e, std::string& out) -> bool;

    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr;

    auto lowerMethodCallImpl(const ast::MethodCall& n) -> ExprPtr;

    auto lowerRecordConstruction(const ast::RecordConstruction& n) -> ExprPtr;

    auto lowerParsedInterpolatedString(
        const std::vector<std::string>& stringParts,
        const std::vector<ast::ExprPtr>& values) -> ExprPtr;

    // ---- IR construction helpers -----------------------------------------
    auto litInt(long v) -> ExprPtr;
    auto sourceLocationTuple() -> ExprPtr;
    auto one(ExprPtr a) -> std::vector<ExprPtr>;
    auto two(ExprPtr a, ExprPtr b) -> std::vector<ExprPtr>;
    auto three(ExprPtr a, ExprPtr b, ExprPtr c) -> std::vector<ExprPtr>;
    auto atomize_ir(ExprPtr e, std::vector<Binding>& binds) -> ExprPtr;
    mutable std::unordered_set<std::string> foulBackendTargets;
    // Module+function+arity combinations positively known to be PURE, which
    // veto the name-level foul mark above.
    mutable std::unordered_set<std::string> pureBackendTargets;
    mutable bool foulBackendTargetsBuilt = false;
    auto isFoulBackendTarget(const std::string& mod, const std::string& fn,
                             int arity) -> bool;

    static auto foulKey(const std::string& mod, const std::string& fn)
        -> std::string;
    static auto foulKey(const std::string& mod, const std::string& fn, int arity)
        -> std::string;

    auto callE(std::string mod, std::string fn, int arity, std::vector<ExprPtr> args) -> ExprPtr;
    auto intrin(Op op, std::vector<ExprPtr> args) -> ExprPtr;

    auto withTestSummary(ExprPtr body) -> ExprPtr;

    // Payload arity per ADT variant tag (nullary variants lower to atoms and
    // never need display info).
    std::unordered_map<std::string, int> variantArity;
    std::unordered_map<std::string, std::string> variantOwner;

    auto withDisplayInfo(ExprPtr body, const std::string& moduleName = "")
        -> ExprPtr;

    // ---- Constructors -----------------------------------------------------
    auto makeLet(std::string name, ExprPtr value, ExprPtr body) -> ExprPtr;
    auto makeMatch1(ExprPtr subject, PatternPtr pattern, ExprPtr body) -> ExprPtr;
    static auto irIsBoolean(const ExprPtr& e) -> bool;

    auto truthyTest(ExprPtr cond) -> ExprPtr;

    auto matchBool(ExprPtr cond, ExprPtr thenE, ExprPtr elseE) -> ExprPtr;

    // ---- Patterns ---------------------------------------------------------
    auto wildPat() -> PatternPtr;

    // ---- Guards -------------------------------------------------------------
    // A match written in source, for the `guards` pass to expand.
    auto sourceMatch(std::vector<ExprPtr> subjects, std::vector<MatchClause> clauses)
        -> ExprPtr;

    // Rescue arms with `when` guards or type tests cannot stay native:
    // the emitter drops TryCatch clause guards, so wrap the arms in one
    // catch-all whose body is the guard expansion with a rethrow failure.
    // Guardless arms pass through untouched. Matches the walker, whose
    // rescue guards skip only on literal `false` (unlike match).
    auto expandRescueGuards(std::vector<MatchClause> clauses)
        -> std::vector<MatchClause>;

    auto fieldAccess(const std::string& fname, ExprPtr base) -> ExprPtr;
    auto fieldAccessIn(const std::string& typeName, const std::string& fname,
                       ExprPtr base) -> ExprPtr;
    void destructureRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                                  std::vector<std::pair<std::string, ExprPtr>>& prefix);
    void collectRecordBindings(const ast::RecordPattern& rp, std::vector<std::string>& names);
    auto wrapRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                           ExprPtr body) -> ExprPtr;
    auto anonymousRecordCandidates(const ast::RecordPattern& rp)
        -> std::vector<std::string>;
    auto anonymousRecordTest(const std::string& subject,
                             const ast::RecordPattern& rp) -> ExprPtr;
    auto anonymousFieldRead(const std::string& subject,
                            const std::vector<std::string>& tags,
                            const std::string& field) -> ExprPtr;
    auto wrapAnonymousRecordPattern(const std::string& subject,
                                    const ast::RecordPattern& rp,
                                    ExprPtr body) -> ExprPtr;
    static auto anonymousRecordPattern(const std::vector<ast::PatternPtr>& pats)
        -> const ast::RecordPattern*;
    auto topLevelPatternBinding(const ast::LetExpr& le, const std::string& name)
        -> ExprPtr;
    // Type tests contributed by `name : Type` patterns in the clause being
    // lowered, waiting to be folded into its guard.
    std::vector<ExprPtr> pendingTypeGuards;

    auto withTypeGuards(ExprPtr existing) -> std::optional<ExprPtr>;

    auto lowerPattern(const ast::PatternPtr& p) -> PatternPtr;

    // ---- Control flow -----------------------------------------------------
    auto lowerBodyScoped(const std::vector<ast::ExprPtr>& body) -> ExprPtr;

    auto lowerIf(const ast::IfExpr& n) -> ExprPtr;

    auto lowerMatch(const ast::MatchExpr& n) -> ExprPtr;

    auto lowerReceive(const ast::ReceiveExpr& n) -> ExprPtr;

    auto lowerRangeMatch(const ast::MatchExpr& n) -> ExprPtr;

    // ---- Loops ------------------------------------------------------------
    void collectMutated(const ast::ExprPtr& e, std::unordered_set<std::string>& out);

    auto requireNoOuterMutation(const std::vector<ast::ExprPtr>& body,
                                const char* what) -> void;

    auto stateExpr(const std::vector<std::string>& mutVars) -> ExprPtr;
    auto tailCall(const std::string& loopFn, const std::vector<std::string>& mutVars) -> ExprPtr;

    auto lowerBindingValue(const ast::LetExpr& le) -> ExprPtr;

    auto loopControl(const ast::ExprPtr& e, const std::string& loopFn,
                     const std::vector<std::string>& mutVars) -> ExprPtr;

    auto lowerLoopBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i,
                           const std::string& loopFn,
                           const std::vector<std::string>& mutVars,
                           const std::function<ExprPtr()>& onEnd) -> ExprPtr;

    auto lowerLoopArmU(const ast::ExprPtr& arm, const std::string& loopFn,
                       const std::vector<std::string>& mutVars,
                       const std::function<ExprPtr()>& armEnd) -> ExprPtr;

    auto lowerMutatingAsValue(const ast::MethodCall& mc) -> ExprPtr;

    auto lowerAssignmentValue(const ast::AssignExpr& assignment) -> ExprPtr;

    auto lowerLoopCore(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       bool restIsLast, const std::function<ExprPtr()>& mkRest,
                       const std::optional<std::string>* counterVar = nullptr) -> ExprPtr;
    auto lowerLoopStmt(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       const std::vector<ast::ExprPtr>& outer, size_t outerStart,
                       const std::optional<std::string>* counterVar = nullptr) -> ExprPtr;

    // The import/alias state `applyUsingBindings` overwrites, so a caller can
    // restore it once whatever scope the bindings apply to is done lowering.
    struct UsingBindingsSaved {
        std::unordered_map<std::string, std::string> imports;
        std::unordered_map<std::string, std::string> aliases;
        std::set<std::string> using_;
        std::unordered_map<std::string, std::string> externalImports;
    };

    auto importExternalModule(const std::string& srcMod,
                              const std::vector<std::string>& only,
                              const std::vector<std::string>& except) -> void;

    auto lowerExternalImportCall(const ast::FunctionCall& n,
                                 const std::string& qualKey) -> ExprPtr;

    auto applyUsingBindings(const ast::UsingExpr& n) -> UsingBindingsSaved;
    auto restoreUsingBindings(UsingBindingsSaved saved) -> void;
    auto snapshotUsingBindings() const -> UsingBindingsSaved;
    auto applyUsingCore(const std::string& srcMod,
                        const std::optional<std::string>& alias,
                        const std::vector<std::string>& onlyNames,
                        const std::vector<std::string>& exceptNames) -> void;

    // ---- Body lowering ----------------------------------------------------
    auto lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr;
    auto lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr;

    // ---- Rescue -----------------------------------------------------------
    auto lowerRescueClauses(const ast::RescueBlock& rescue, bool blockValue = false)
        -> std::vector<MatchClause>;

    auto wrapWithTryCatch(ExprPtr body, const ast::RescueBlock& rescue,
                          bool blockValue = false) -> ExprPtr;

    static auto irHasEscapingTryThrow(const ExprPtr& e) -> bool;

    auto wrapPropagateTryError(ExprPtr body) -> ExprPtr;

    // ---- Function / program ----------------------------------------------
    auto lowerParam(const ast::Param& p) -> PatternPtr;
    auto typeNameParameter(const ast::Pattern& p) -> PatternPtr;

    auto discriminatingType(const std::string& written) const -> bool;

    auto lowerFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                            const std::string& implicitThisName = "",
                            const std::string& nameOverride = "") -> FunDef;

    // ---- Records ----------------------------------------------------------
    void addFieldAccessor(const std::string& field, const std::string& record,
                          int position, int tupleSize, bool local);

    void collectRecordLayout(const std::string& name,
                             const std::vector<std::string>& fields);

    void collectRecord(const ast::RecordDef& rec,
                       const std::string& owner = {});

    auto externalPrefixCall(const std::string& name, int arity) const
        -> std::string;


    // A call the value decides among the providers the checker listed
    // (ResolvedCallTarget::alternatives).
    auto providerChoice(const ast::MethodCall& n, const semantic::ResolvedCallTarget& target)
        -> ExprPtr;


    auto fieldAccessorDelegate(const std::string& field, bool& blocked) const
        -> const ExternalModules::ReceiverFunction*;

    auto makeAccessors(const std::unordered_set<std::string>& definedFns,
                       const std::unordered_set<std::string>& definedFnArities)
        -> std::vector<FunDef>;

    // ---- Make blocks ------------------------------------------------------
    auto lowerMakeGroup(const std::vector<const ast::FunctionDef*>& group,
                        const std::string& typeName) -> FunDef;

    auto coerceListReceiver(FunDef def) -> FunDef;

    static auto primGuardBifs() -> const std::unordered_map<std::string, const char*>&;
    // The runtime shapes a value of `owner` can have, as `typeGuard` tells
    // them apart (`integer`, `binary`, `tuple:Point`, `atom:None`, ...), or
    // nothing when they are not known. Two owners whose shapes meet cannot
    // both be routed by guard order alone.
    auto runtimeShapes(const std::string& owner)
        -> std::optional<std::set<std::string>>;
    // The tag typeGuard tests a non-record tagged type by: an opaque type
    // declared in a module carries its qualified name.
    auto guardTag(const std::string& canonical) const -> std::string;
    static auto shapesMeet(const std::optional<std::set<std::string>>& a,
                           const std::optional<std::set<std::string>>& b) -> bool;

    auto recordGuard(const FieldSlot& slot, ExprPtr v) -> ExprPtr;

    auto adtGuard(const std::vector<std::string>& tags, ExprPtr v) -> ExprPtr;


    auto typeGuard(const std::string& written, ExprPtr v) -> ExprPtr;

    auto registerModuleAliases(const std::string& srcMod,
                               const std::vector<std::string>& onlyNames,
                               const std::vector<std::string>& exceptNames)
        -> void;

    auto makeArgumentDispatcher(
        const std::string& name, int arity,
        const std::vector<std::pair<std::string, std::vector<std::string>>>&
            overloads,
        const std::string& fallbackModule = {}) -> FunDef;

    auto makeDispatcher(const std::string& name, int arity,
                        const std::vector<std::string>& owners,
                        const std::string& genericFallback = {},
                        const std::string& genericFallbackModule = {},
                        int genericFallbackArity = -1,
                        bool contextualOwners = false) -> FunDef;

    auto makeListOf(ExprPtr first, ExprPtr second) -> ExprPtr;

    auto appendFieldClauses(const std::string& name, int arity, Match& m) -> void;
};

} // namespace kex::ir

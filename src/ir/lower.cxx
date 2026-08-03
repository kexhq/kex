#include "lower.hxx"
#include "../common/type_def_utils.hxx"
#include "../lexer/token.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace kex::ir {

namespace {

// Backend-local symbol construction lives here so every producer and reader
// changes together. Dots identify qualified/module membership; slash
// introduces a receiver/argument overload signature.
auto mangleQualifiedMember(const std::string& scope,
                           const std::string& member) -> std::string {
    return scope + "." + member;
}

auto mangleReceiverImplementation(const std::string& method,
                                  const std::string& receiverType)
    -> std::string {
    return method + "/" + receiverType;
}

auto mangleReceiverSignature(const std::string& method,
                             const std::vector<std::string>& dispatchTypes)
    -> std::string {
    std::string out = method + "/";
    for (size_t i = 0; i < dispatchTypes.size(); i++) {
        if (i) out += ",";
        out += dispatchTypes[i];
    }
    return out;
}

auto receiverImplementationPrefix(const std::string& method) -> std::string {
    return method + "/";
}

auto mangleModulePath(const std::string& path) -> std::string {
    return path;
}

auto mangleModuleMember(const std::string& path,
                        const std::string& member) -> std::string {
    return mangleQualifiedMember(mangleModulePath(path), member);
}

auto renderDispatchType(const ast::TypeExpr& type) -> std::string {
    return std::visit([](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            return node.parts.empty() ? "Any" : node.parts.back();
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string out =
                node.name.parts.empty() ? "Any" : node.name.parts.back();
            out += "<";
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i) out += ", ";
                out += node.args[i] ? renderDispatchType(*node.args[i]) : "Any";
            }
            return out + ">";
        } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            return "(" +
                (node.param ? renderDispatchType(*node.param) : "Any") +
                ") -> " +
                (node.result ? renderDispatchType(*node.result) : "Any");
        } else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::string out = "(";
            for (size_t i = 0; i < node.elements.size(); i++) {
                if (i) out += ", ";
                out += node.elements[i]
                    ? renderDispatchType(*node.elements[i]) : "Any";
            }
            return out + ")";
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            auto element =
                node.element ? renderDispatchType(*node.element) : "Any";
            return element == "Char" ? "String" : "[" + element + "]";
        } else if constexpr (std::is_same_v<T, ast::MapType>) {
            return "{" +
                (node.key ? renderDispatchType(*node.key) : "Any") + ": " +
                (node.value ? renderDispatchType(*node.value) : "Any") + "}";
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            return (node.left ? renderDispatchType(*node.left) : "Any") +
                " | " +
                (node.right ? renderDispatchType(*node.right) : "Any");
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return (node.inner ? renderDispatchType(*node.inner) : "Any") + "?";
        } else if constexpr (std::is_same_v<T, ast::BlockType>) {
            return "Block<" +
                (node.inner ? renderDispatchType(*node.inner) : "Any") + ">";
        } else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return "Atom";
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return node.name;
        }
        return "Any";
    }, type.kind);
}

auto methodDispatchTypes(const ast::FunctionDef& function,
                         const std::string& receiverType)
    -> std::vector<std::string> {
    std::vector<std::string> types{receiverType};
    if (function.clauses.empty()) return types;
    for (const auto& param : function.clauses.front().params)
        types.push_back(param.type && *param.type
            ? renderDispatchType(**param.type)
            : "Any");
    return types;
}

auto localOverloadKey(const std::string& method,
                      const std::string& receiverType,
                      size_t arity) -> std::string {
    return method + "\n" + receiverType + "\n" + std::to_string(arity);
}

// Does `expr` reference the SSA name `name` anywhere? Names are freshened at
// binding time, so a plain syntactic scan is exact — nothing shadows.
auto mentionsVar(const Expr& expr, const std::string& name) -> bool {
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        auto visit = [&](const ExprPtr& child) {
            return child && mentionsVar(*child, name);
        };
        auto anyOf = [&](const std::vector<ExprPtr>& items) {
            for (const auto& item : items) if (visit(item)) return true;
            return false;
        };
        auto anyClause = [&](const std::vector<MatchClause>& clauses) {
            for (const auto& clause : clauses) {
                if (clause.guard && visit(*clause.guard)) return true;
                if (visit(clause.body)) return true;
            }
            return false;
        };
        if constexpr (std::is_same_v<T, Var>) {
            return node.name == name;
        } else if constexpr (std::is_same_v<T, Intrinsic>) {
            return anyOf(node.args);
        } else if constexpr (std::is_same_v<T, Call>) {
            return anyOf(node.args);
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            return visit(node.callee) || anyOf(node.args);
        } else if constexpr (std::is_same_v<T, Let>) {
            return visit(node.value) || visit(node.body);
        } else if constexpr (std::is_same_v<T, Seq>) {
            return anyOf(node.exprs);
        } else if constexpr (std::is_same_v<T, Match>) {
            return anyOf(node.subjects) || anyClause(node.clauses);
        } else if constexpr (std::is_same_v<T, Construct>) {
            return anyOf(node.args);
        } else if constexpr (std::is_same_v<T, MakeTuple>) {
            return anyOf(node.elements);
        } else if constexpr (std::is_same_v<T, MakeList>) {
            return anyOf(node.elements) || (node.rest && visit(*node.rest));
        } else if constexpr (std::is_same_v<T, FieldGet>) {
            return visit(node.record);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            return visit(node.body);
        } else if constexpr (std::is_same_v<T, Return>) {
            return visit(node.value);
        } else if constexpr (std::is_same_v<T, TryThrow>) {
            return visit(node.error);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            return visit(node.body) || anyClause(node.clauses);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            return visit(node.funBody) || visit(node.contBody);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (const auto& clause : node.clauses)
                if (visit(clause.body)) return true;
            return (node.timeout && visit(*node.timeout)) ||
                   (node.afterBody && visit(*node.afterBody));
        } else {
            return false;
        }
    }, expr.node);
}

// A pending `let name = value in ...` binding, accumulated while lowering a
// compound expression's operands into ANF, then wrapped (outermost-first)
// around the consuming expression.
struct Binding {
    std::string name;
    ExprPtr value;
};

struct Lowering {
    int counter = 0;
    std::string sourceFile;
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
    };
    std::unordered_map<std::string, RecordInfo> records;
    // field name → [(record name, 1-based position)]. A field can live in
    // several records at (possibly different) positions; the emitted accessor
    // dispatches on the tag when they differ. Mirrors the string emitter.
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> fieldAccessors;
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
    // Make-method trailing defaults, indexed by explicit parameter position.
    std::unordered_map<std::string, std::vector<const ast::ExprPtr*>> methodDefaults;
    // Cross-type method-name collisions: a method name → the make-block type
    // names that define it, in order. When more than one type defines the
    // same name, each type's version is emitted mangled (`name/Type`) and a
    // dispatcher under the bare name selects at runtime on the receiver's tag
    // (element 1). Mirrors the string emitter's collision handling.
    std::unordered_map<std::string, std::vector<std::string>> methodOwners;
    std::unordered_set<std::string> collidingMethods;
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
    // Record/make type names (so a namespace call can be recognized as a
    // static method dispatch, not an unknown module).
    std::unordered_set<std::string> knownTypes;
    // Top-level `let name = expr` bindings become 0-arity functions; a bare
    // reference to one compiles to `apply 'name'/0()`, not a variable.
    std::unordered_set<std::string> topLevelConstants;
    // Top-level zero-parameter functions (e.g. `foul name do ... end`). A bare
    // reference to one (no parens) is a call `apply 'name'/0()`, not a var.
    std::unordered_set<std::string> zeroArgFns;
    // Top-level free function → its ordered parameter names ("" for an
    // unnamed/pattern param). Lets a call with named args reorder them into
    // positional slots.
    std::unordered_map<std::string, std::vector<std::string>> fnParamNames;
    // Names that are real functions in this module (top-level + make methods).
    // A call to a name NOT in here is an indirect apply through a variable
    // holding a fun (e.g. a `block` parameter).
    std::unordered_set<std::string> knownFns;
    // Qualified Kex module function (`Util.Math.double`) -> local BEAM name.
    // Modules in a single source file still flatten into one BEAM module, so
    // this preserves qualification without a cross-module call.
    std::unordered_map<std::string, std::string> moduleFunctions;
    // Current module path while lowering a module body. Nested module-relative
    // qualified calls (`Router.get` inside `module Http`) resolve against it.
    std::string currentModulePath;
    // Receiver type of the make block currently being lowered. Record field
    // names are not globally unique, so `this.field` must select the layout
    // owned by this type rather than the first same-named field in the file.
    std::string currentMakeType;
    // Bare imported function name → mangled name. Populated by `using M, only:`
    // inside module bodies so bare calls resolve to the correct cross-module fn.
    std::unordered_map<std::string, std::string> moduleImports;
    // Lexically visible `using Source, as: Alias` mappings. The receiver's
    // first path segment is expanded before qualified module lookup.
    std::unordered_map<std::string, std::string> moduleAliases;
    // Loaded external modules from KexI registry (/load).
    const ExternalModules* externalModules = nullptr;
    // When true, external receiver functions take priority over localMethods.
    // Set during prelude self-compilation so internal receiver calls route
    // through the module's own type-dispatching wrappers instead of the
    // inline block/HOF lowerings.
    bool preferExternalReceivers = false;
    // Exact imported call ownership selected by semantic analysis.
    const std::unordered_map<const ast::MethodCall*,
        semantic::ResolvedCallTarget>* resolvedCalls = nullptr;
    const StaticTypeOfCalls* staticTypeOfCalls = nullptr;
    // ADT/variant type → its tag names (e.g. Optional → {"Just","None"}). Used
    // by the dispatcher to wildcard-match any variant of a type, not just the
    // type name itself (which isn't set as element(1) on any variant value).
    std::unordered_map<std::string, std::vector<std::string>> typeVariantTags;
    // Nullary variant tags (no payload) — dispatched as atoms, not tuples.
    std::unordered_set<std::string> nullaryVariantTags;
    // Every variant tag of every ADT (flat), and every record static-block
    // member name. A bare use of a static-only name is out of scope — it
    // lives behind its record (Temperature.Fahrenheit) — so it must be a
    // runtime error like the walker's, not a silent constructor tuple.
    std::unordered_set<std::string> variantTagSet;
    std::unordered_set<std::string> staticCtorNames;
    auto staticCtorOutOfScope(const std::string& name) -> bool {
        return staticCtorNames.count(name) && !variantTagSet.count(name)
            && !records.count(name) && !knownFns.count(name);
    }
    // `Mod.Tag` for an ADT declared inside a module (e.g. `Kex.FS`). A tag is
    // just an atom at runtime, so the qualifier has no representation here —
    // resolve to the bare tag. `variantOwner` covers tags declared outside
    // this module (the prelude's, via externalVariants) as well as its own,
    // so an unknown name still falls through to the usual error.
    auto qualifiedVariantTag(const std::string& name) const -> std::string {
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return "";
        auto tag = name.substr(dot + 1);
        if (tag.empty() || !std::isupper(static_cast<unsigned char>(tag[0])))
            return "";
        return (variantTagSet.count(tag) || variantOwner.count(tag)) ? tag : "";
    }
    // A semantic::StructuredType as the `Type { name, args }` record it
    // stands for — a tagged tuple, the same shape a record literal lowers to.
    auto structuredTypeLiteral(const semantic::StructuredType& type) -> ExprPtr {
        std::vector<ExprPtr> args;
        for (const auto& arg : type.args) args.push_back(structuredTypeLiteral(arg));
        auto list = std::make_unique<Expr>();
        list->node = MakeList{std::move(args), std::nullopt};
        auto record = std::make_unique<Expr>();
        std::vector<ExprPtr> fields;
        fields.push_back(lit(LitKind::String, type.name));
        fields.push_back(std::move(list));
        fields.push_back(litBool(type.pure));
        record->node = Construct{"Type", std::move(fields)};
        return record;
    }

    // An erlang:error carrying the walker's exact runtime-error text,
    // prefixed with the current source location.
    auto runtimeError(const std::string& msg) -> ExprPtr {
        std::string loc;
        if (currentLoc) loc = std::string(currentLoc->file) + ":"
            + std::to_string(currentLoc->line) + ":"
            + std::to_string(currentLoc->column) + ": ";
        return callE("erlang", "error", 1, one(lit(LitKind::String,
            loc + "runtime error: " + msg)));
    }

    static auto opSymbol(TokenType t) -> std::string {
        switch (t) {
            case TokenType::Plus: return "+";     case TokenType::Minus: return "-";
            case TokenType::Star: return "*";     case TokenType::Slash: return "/";
            case TokenType::Percent: return "%";  case TokenType::Caret: return "^";
            case TokenType::EqEq: return "==";
            case TokenType::NotEq: return "!=";   case TokenType::LessThan: return "<";
            case TokenType::GreaterThan: return ">"; case TokenType::LessEq: return "<=";
            case TokenType::GreaterEq: return ">="; default: return "";
        }
    }

    static auto simpleTypeName(const ast::TypeExprPtr& t) -> std::string {
        if (!t) return "";
        if (auto* tn = std::get_if<ast::TypeName>(&t->kind))
            if (!tn->parts.empty()) return tn->parts.back();
        if (auto* g = std::get_if<ast::GenericType>(&t->kind))
            if (!g->name.parts.empty()) return g->name.parts.back();
        // List/Map types get canonical names so list-vs-map methods (the
        // polymorphic HOFs) collide and dispatch by runtime type (is_list/
        // is_map) instead of falling into separate un-mangled `map/2`s.
        if (std::holds_alternative<ast::ListType>(t->kind)) return "List";
        if (std::holds_alternative<ast::MapType>(t->kind)) return "Map";
        return "";
    }
    // SSA renaming: Kex source name → its current IR variable name. A `let`/
    // `var`/reassignment introduces a fresh name and updates this; Identifier
    // lowering consults it. Function params map to themselves (the emitter
    // uppercases). This is the SSA construction the string emitter did via
    // m_varSubst, now done once in the lowering pass.
    std::unordered_map<std::string, std::string> subst;

    auto fresh(const std::string& hint = "T") -> std::string {
        return "_ir_" + hint + std::to_string(counter++);
    }

    auto currentName(const std::string& kexName) -> std::string {
        auto it = subst.find(kexName);
        return it != subst.end() ? it->second : kexName;
    }

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
    auto snap(const ExprPtr& e) -> AtomicRef {
        if (e) {
            if (auto* v = std::get_if<Var>(&e->node)) return {v->name, std::nullopt};
            if (auto* l = std::get_if<Lit>(&e->node)) return {"", *l};
        }
        return {"_snap_null", std::nullopt};
    }

    auto clone(const ExprPtr& e, const char* site = "?") -> ExprPtr {
        if (!e) throw LowerError(std::string("IR lower: clone of null expr [") + site + "]");
        if (auto* v = std::get_if<Var>(&e->node)) return var(v->name);
        if (auto* l = std::get_if<Lit>(&e->node)) {
            auto out = std::make_unique<Expr>(); out->node = *l; return out;
        }
        if (auto* c = std::get_if<Construct>(&e->node)) {
            if (c->args.empty()) {
                auto out = std::make_unique<Expr>();
                out->node = Construct{c->tag, {}};
                return out;
            }
        }
        throw LowerError(std::string("IR lower: clone of non-atomic expr (index ")
            + std::to_string(e->node.index()) + ") [" + site + "]");
    }

    // Wrap `body` in the accumulated let-bindings, evaluated left-to-right
    // (so binds[0] is the outermost let).
    auto wrapLets(std::vector<Binding>& binds, ExprPtr body) -> ExprPtr {
        for (auto it = binds.rbegin(); it != binds.rend(); ++it) {
            auto let = std::make_unique<Expr>();
            let->node = Let{std::move(it->name), std::move(it->value), std::move(body)};
            body = std::move(let);
        }
        return body;
    }

    static auto isAtomic(const Expr& e) -> bool {
        return std::holds_alternative<Lit>(e.node) || std::holds_alternative<Var>(e.node);
    }

    // Lower an AST expr and, if the result is compound, bind it to a fresh
    // Let (recorded in `binds`) returning an atomic Var. Literals/vars pass
    // through untouched. This is the ANF normalization step.
    auto atomize(const ast::ExprPtr& e, std::vector<Binding>& binds) -> ExprPtr {
        auto ir = lower(e);
        if (isAtomic(*ir)) return ir;
        auto name = fresh();
        binds.push_back({name, std::move(ir)});
        return var(name);
    }


    auto opOf(TokenType t) -> Op {
        switch (t) {
            case TokenType::Plus: return Op::Add;
            case TokenType::Minus: return Op::Sub;
            case TokenType::Star: return Op::Mul;
            case TokenType::Slash: return Op::Div;
            case TokenType::Percent: return Op::Mod;
            case TokenType::Caret: return Op::Pow;
            case TokenType::EqEq: return Op::Eq;
            case TokenType::NotEq: return Op::Neq;
            case TokenType::LessThan: return Op::Lt;
            case TokenType::GreaterThan: return Op::Gt;
            case TokenType::LessEq: return Op::Lte;
            case TokenType::GreaterEq: return Op::Gte;
            case TokenType::AmpAmp: return Op::And;
            case TokenType::PipePipe: return Op::Or;
            default:
                throw LowerError("IR lower: unsupported binary operator");
        }
    }

    // ---- Expression lowering ---------------------------------------------
    auto lower(const ast::ExprPtr& e) -> ExprPtr {
        if (!e) return litBool(false);
        auto prevLoc = currentLoc;
        currentLoc = &e->location;
        auto r = std::visit([&](const auto& n) -> ExprPtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::IntLiteral>) {
                return lit(LitKind::Int, n.value);
            } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
                return lit(LitKind::Float, n.value);
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                if (!n.parts.empty())
                    return lowerParsedInterpolatedString(n.parts, n.values);
                if (n.interpolating && n.value.find("${") != std::string::npos)
                    return lowerInterpolatedString(n.value);
                return lit(LitKind::String, n.value);
            } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
                return litBool(n.value);
            } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
                return lit(LitKind::Char, std::string(1, n.value));
            } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
                return lit(LitKind::Atom, n.name);
            } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
                return lit(LitKind::None, "none");
            } else if constexpr (std::is_same_v<T, ast::Identifier>) {
                // A bare reference to a top-level `let` constant (not shadowed
                // by a local) is a call to its 0-arity function.
                if (!subst.count(n.name) &&
                    (topLevelConstants.count(n.name) || zeroArgFns.count(n.name))) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"", n.name, 0, {}, false};
                    return ex;
                }
                // Uppercase bare name not a known constant → nullary ADT
                // constructor (matching the UpperIdentifier path). Without
                // this, uppercase names in expression context become unbound
                // variables in Core Erlang and erlc rejects them.
                if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))) {
                    if (staticCtorOutOfScope(n.name))
                        return runtimeError("Undefined identifier: " + n.name);
                    auto ex = std::make_unique<Expr>();
                    ex->node = Construct{n.name, {}};
                    return ex;
                }
                if (auto it = subst.find(n.name); it != subst.end())
                    return var(it->second);
                if (knownFns.count(n.name))
                    return var(n.name);
                // Genuinely unbound: every binding site registers itself in
                // `subst`, so absence means the walker would raise at runtime —
                // emit the same error so `it`-caught failures match exactly.
                return runtimeError("Undefined variable: " + n.name);
            } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
                return var(currentName("this"));
            } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
                // An all-caps top-level constant (e.g. DEFAULT_LEVEL, defined
                // via `let NAME = value` → a 0-arity function) is a call, not a
                // nullary ADT constructor.
                if (!subst.count(n.name) &&
                    (topLevelConstants.count(n.name) || zeroArgFns.count(n.name))) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"", n.name, 0, {}, false};
                    return ex;
                }
                // Bare capitalized name = nullary ADT constructor / None-like
                // tag → the atom 'Name' (e.g. JsonNull, Less).
                auto ex = std::make_unique<Expr>();
                if (auto tag = qualifiedVariantTag(n.name); !tag.empty()) {
                    ex->node = Construct{tag, {}};
                    return ex;
                }
                if (staticCtorOutOfScope(n.name))
                    return runtimeError("Undefined identifier: " + n.name);
                ex->node = Construct{n.name, {}};
                return ex;
            } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                return lowerRecordConstruction(n);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                // && / || MUST short-circuit: the right operand is evaluated
                // lazily, so it cannot be ANF-atomized (that would force it).
                // Lower to a boolean Match instead, matching the tree-walker
                // (spec/mutual_recursion.kex relies on `n == 0 || isOdd(n-1)`
                // never recursing once n == 0).
                if (n.op == TokenType::AmpAmp) {
                    return matchBool(lower(n.left), lower(n.right), litBool(false));
                }
                if (n.op == TokenType::PipePipe) {
                    return matchBool(lower(n.left), litBool(true), lower(n.right));
                }
                // Division by literal zero → compile-time error with location,
                // matching the walker's runtime error format.
                if (n.op == TokenType::Slash && n.right) {
                    if (auto* il = std::get_if<ast::IntLiteral>(&n.right->kind)) {
                        if (il->value == "0") {
                            std::string loc;
                            if (currentLoc) loc = std::string(currentLoc->file) + ":"
                                + std::to_string(currentLoc->line) + ":"
                                + std::to_string(currentLoc->column) + ": ";
                            return callE("erlang", "error", 1, one(
                                lit(LitKind::String, loc + "runtime error: Division by zero")));
                        }
                    }
                }
                std::vector<Binding> binds;
                auto l = atomize(n.left, binds);
                auto r = atomize(n.right, binds);
                // Operator overloading: if this operator is defined by a make
                // block, dispatch on the LHS at runtime — a tuple (record)
                // uses the user's `'op'/2`, anything else the builtin op
                // (spec/operator_overloading.kex).
                std::string sym = opSymbol(n.op);
                // User == / != (outside guards, which only allow BIFs) go
                // through the coercing runtime eq: a [Char] charlist and a
                // String binary holding the same text ARE equal in Kex.
                auto builtinOp = [&](ExprPtr a, ExprPtr b) -> ExprPtr {
                    Op op = opOf(n.op);
                    if (op == Op::Eq || op == Op::Neq)
                        return callE("kex_intrinsic_number",
                                     op == Op::Eq ? "eq" : "neq", 2,
                                     two(std::move(a), std::move(b)));
                    auto ex = std::make_unique<Expr>();
                    ex->node = Intrinsic{op, two(std::move(a), std::move(b))};
                    return ex;
                };
                if (!sym.empty() && overloadedOps.count(sym)) {
                    auto lRef = snap(l); auto rRef = snap(r);
                    auto builtin = builtinOp(lRef.get(), rRef.get());
                    std::vector<ExprPtr> ua; ua.push_back(lRef.get()); ua.push_back(rRef.get());
                    std::string userModule;
                    std::string userFunction = sym;
                    // A locally-defined operator stays a local apply. When the
                    // overload comes from an imported receiver interface
                    // (notably the prelude), route it to that provider module.
                    if (!knownFns.count(sym) && externalModules) {
                        if (auto it = externalModules->receiverFunctions.find(sym);
                            it != externalModules->receiverFunctions.end()) {
                            for (const auto& candidate : it->second) {
                                if (candidate.beamArity != 2) continue;
                                userModule = candidate.moduleAtom;
                                userFunction = candidate.beamFunction;
                                break;
                            }
                        }
                    }
                    auto userCall = std::make_unique<Expr>();
                    userCall->node = Call{
                        std::move(userModule), std::move(userFunction),
                        2, std::move(ua), false};
                    // Nullary ADT values such as `Watt` are atoms rather
                    // than tuples. If their declared type owns this operator,
                    // statically select that overload instead of falling
                    // through to Erlang arithmetic.
                    if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.left->kind)) {
                        if (auto owner = variantOwner.find(uid->name);
                            owner != variantOwner.end()) {
                            if (auto methods = methodOwners.find(sym);
                                methods != methodOwners.end() &&
                                std::find(methods->second.begin(), methods->second.end(),
                                          owner->second) != methods->second.end())
                                return wrapLets(binds, std::move(userCall));
                        }
                    }
                    auto dispatch = matchBool(
                        callE("erlang", "is_tuple", 1, one(lRef.get())),
                        std::move(userCall), std::move(builtin));
                    return wrapLets(binds, std::move(dispatch));
                }
                return wrapLets(binds, builtinOp(std::move(l), std::move(r)));
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                std::vector<Binding> binds;
                auto a = atomize(n.operand, binds);
                Op op = (n.op == TokenType::Minus) ? Op::Neg
                       : (n.op == TokenType::Bang) ? Op::Not
                       : throw LowerError("IR lower: unsupported unary operator");
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(a));
                ex->node = Intrinsic{op, std::move(args)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
                // a..b → a materialized ascending list. kex_intrinsic_range
                // wraps lists:seq and handles Char endpoints ({'Char', N}
                // bounds produce a [Char], not an [Int]).
                std::vector<Binding> binds;
                auto s = atomize(n.start, binds);
                auto en = atomize(n.end, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(s));
                args.push_back(std::move(en));
                ex->node = Call{"kex_intrinsic_range", "make", 2, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
                std::vector<Binding> binds;
                std::vector<ExprPtr> els;
                for (const auto& el : n.elements) els.push_back(atomize(el, binds));
                auto ex = std::make_unique<Expr>();
                ex->node = MakeTuple{std::move(els)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                std::vector<Binding> binds;
                bool hasSpreads = false;
                for (const auto& el : n.elements)
                    if (el && std::holds_alternative<ast::SpreadExpr>(el->kind)) { hasSpreads = true; break; }

                if (!hasSpreads) {
                    std::vector<ExprPtr> els;
                    for (const auto& el : n.elements) els.push_back(atomize(el, binds));
                    std::optional<ExprPtr> rest;
                    if (n.rest) rest = atomize(*n.rest, binds);
                    auto ex = std::make_unique<Expr>();
                    ex->node = MakeList{std::move(els), std::move(rest)};
                    return wrapLets(binds, std::move(ex));
                }
                // [a, ...b, c, ...d] → lists:append([a], lists:append(b, [c | d]))
                // Build segments: each run of non-spread elements forms a literal
                // list, each spread is its own list. Concatenate with lists:append.
                std::vector<ExprPtr> segments;
                std::vector<ExprPtr> currentRun;
                auto flushRun = [&]() {
                    if (currentRun.empty()) return;
                    auto lst = std::make_unique<Expr>();
                    lst->node = MakeList{std::move(currentRun), std::nullopt};
                    segments.push_back(std::move(lst));
                    currentRun.clear();
                };
                for (const auto& el : n.elements) {
                    if (el && std::holds_alternative<ast::SpreadExpr>(el->kind)) {
                        flushRun();
                        segments.push_back(lower(std::get<ast::SpreadExpr>(el->kind).inner));
                    } else {
                        currentRun.push_back(atomize(el, binds));
                    }
                }
                flushRun();
                // Fold right with lists:append
                ExprPtr result = std::move(segments.back()); segments.pop_back();
                while (!segments.empty()) {
                    auto left = std::move(segments.back()); segments.pop_back();
                    std::vector<ExprPtr> args;
                    args.push_back(std::move(left));
                    args.push_back(std::move(result));
                    result = callE("lists", "append", 2, std::move(args));
                }
                return wrapLets(binds, std::move(result));
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                return lowerIf(n);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                return lowerMatch(n);
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                // `expr if cond` → cond ? expr : ok
                return matchBool(lower(n.condition), lower(n.expr), lit(LitKind::Atom, "ok"));
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                return matchBool(lower(n.condition), lower(n.thenExpr), lower(n.elseExpr));
            } else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
                // `&.method` / `&.method(args)` → fun(_sx) -> _sx.method(args).
                return lowerNameAsUfcsFun(n.name, n.args);
            } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                if (n.entries.empty()) return callE("maps", "new", 0, {});
                std::vector<Binding> binds;
                // Runs of literal pairs become one `maps:from_list`; each
                // `...other` is a map in its own right. Folding the segments
                // with `maps:merge` gives later-wins, since merge/2 lets its
                // SECOND argument take precedence.
                std::vector<ExprPtr> segments;
                std::vector<ExprPtr> pairs;
                auto flushPairs = [&]() {
                    if (pairs.empty()) return;
                    auto lst = std::make_unique<Expr>();
                    lst->node = MakeList{std::move(pairs), std::nullopt};
                    pairs.clear();
                    segments.push_back(
                        callE("maps", "from_list", 1, one(std::move(lst))));
                };
                for (const auto& ent : n.entries) {
                    if (ent.spread) {
                        flushPairs();
                        segments.push_back(atomize(ent.value, binds));
                        continue;
                    }
                    auto t = std::make_unique<Expr>();
                    t->node = MakeTuple{two(atomize(ent.key, binds), atomize(ent.value, binds))};
                    pairs.push_back(std::move(t));
                }
                flushPairs();
                auto merged = std::move(segments.front());
                for (size_t i = 1; i < segments.size(); i++)
                    merged = callE("maps", "merge", 2,
                                   two(std::move(merged), std::move(segments[i])));
                return wrapLets(binds, std::move(merged));
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                // Params shadow outer bindings: resolve to themselves inside.
                auto snap = subst;
                Lambda lam;
                for (const auto& p : n.params) { lam.params.push_back(p.name); subst[p.name] = p.name; }
                lam.body = lowerBody(n.body);
                if (n.rescue) lam.body = wrapWithTryCatch(std::move(lam.body), *n.rescue);
                subst = snap;
                auto ex = std::make_unique<Expr>();
                ex->node = std::move(lam);
                return ex;
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                auto ex = std::make_unique<Expr>();
                ex->node = Return{lower(n.value)};
                return ex;
            } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                return lowerCurry(n);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                // A `do ... end` block as an expression (e.g. a match arm) is
                // just its body sequence, scoped so its bindings don't leak.
                return lowerBodyScoped(n.body);
            } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                // spawn do B end → erlang:spawn(fun() -> B end).
                Lambda lam; lam.body = lowerBody(n.body);
                auto fn = std::make_unique<Expr>(); fn->node = std::move(lam);
                std::vector<Binding> binds;
                auto fnv = atomize_ir(std::move(fn), binds);
                return wrapLets(binds, callE("erlang", "spawn", 1, one(std::move(fnv))));
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                return lowerReceive(n);
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                return lowerFunctionCall(n);
            } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                std::vector<ExprPtr> partItems;
                for (const auto& part : n.parts) {
                    partItems.push_back(lit(LitKind::String, part));
                }
                auto parts = std::make_unique<Expr>();
                parts->node = MakeList{std::move(partItems), std::nullopt};

                std::vector<ExprPtr> valueItems;
                for (const auto& value : n.values) {
                    if (value) valueItems.push_back(lower(value));
                }
                auto values = std::make_unique<Expr>();
                values->node = MakeList{std::move(valueItems), std::nullopt};

                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                args.push_back(atomize_ir(std::move(parts), binds));
                args.push_back(atomize_ir(std::move(values), binds));
                auto call = std::make_unique<Expr>();
                if (knownFns.count(n.tag))
                    call->node = Call{"", n.tag, 2, std::move(args), false};
                else if (auto imp = moduleImports.find(n.tag);
                         imp != moduleImports.end())
                    call->node =
                        Call{"", imp->second, 2, std::move(args), false};
                else if (subst.count(n.tag))
                    call->node = CallIndirect{
                        var(currentName(n.tag)), std::move(args), false};
                else
                    return wrapLets(
                        binds, runtimeError("Undefined function: " + n.tag));
                return wrapLets(binds, std::move(call));
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                return lowerMethodCall(n);
            } else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
                std::string srcMod;
                for (size_t i = 0; i < n.module.parts.size(); i++) {
                    if (i) srcMod += ".";
                    srcMod += n.module.parts[i];
                }
                auto saved = moduleImports;
                auto savedAliases = moduleAliases;
                if (n.alias) moduleAliases[*n.alias] = srcMod;
                if (!n.onlyNames.empty()) {
                    for (const auto& name : n.onlyNames) {
                        auto key = srcMod + "." + name;
                        if (auto it = moduleFunctions.find(key); it != moduleFunctions.end())
                            moduleImports[name] = it->second;
                    }
                } else {
                    for (const auto& [key, val] : moduleFunctions)
                        if (key.rfind(srcMod + ".", 0) == 0) {
                            auto bare = key.substr(srcMod.size() + 1);
                            if (bare.find('.') == std::string::npos
                                && std::find(n.exceptNames.begin(), n.exceptNames.end(), bare)
                                    == n.exceptNames.end())
                                moduleImports[bare] = val;
                        }
                }
                auto result = lowerBody(n.body);
                moduleImports = std::move(saved);
                moduleAliases = std::move(savedAliases);
                return result;
            } else if constexpr (std::is_same_v<T, ast::TryExpr>) {
                // .try desugars to: case operand of Ok(v) -> v; Error(e) -> return Error(e)
                auto subject = lower(n.operand);
                std::vector<Binding> binds;
                auto sAtom = atomize_ir(std::move(subject), binds);
                auto okVar = fresh("_try_ok");
                auto errVar = fresh("_try_err");

                auto mkConstructPat = [](const std::string& tag, std::vector<PatternPtr> args) {
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Construct;
                    p->tag = tag;
                    p->args = std::move(args);
                    return p;
                };
                auto mkVarPat = [](const std::string& name) {
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Var;
                    p->name = name;
                    return p;
                };
                auto mkConstruct = [&](const std::string& tag, std::vector<ExprPtr> args) {
                    auto e = std::make_unique<Expr>();
                    e->node = Construct{tag, std::move(args)};
                    return e;
                };

                // Ok(v) -> v
                MatchClause okClause;
                std::vector<PatternPtr> okArgs; okArgs.push_back(mkVarPat(okVar));
                okClause.patterns.push_back(mkConstructPat("Ok", std::move(okArgs)));
                okClause.body = var(okVar);
                // Error(e) -> throw try error
                MatchClause errClause;
                std::vector<PatternPtr> errArgs; errArgs.push_back(mkVarPat(errVar));
                errClause.patterns.push_back(mkConstructPat("Error", std::move(errArgs)));
                auto throwExpr = std::make_unique<Expr>();
                throwExpr->node = TryThrow{var(errVar)};
                errClause.body = std::move(throwExpr);
                // Just(v) -> v
                auto justVar = fresh("_try_just");
                MatchClause justClause;
                std::vector<PatternPtr> justArgs; justArgs.push_back(mkVarPat(justVar));
                justClause.patterns.push_back(mkConstructPat("Just", std::move(justArgs)));
                justClause.body = var(justVar);
                // None -> throw try error(none)
                MatchClause noneClause;
                noneClause.patterns.push_back(mkConstructPat("None", {}));
                auto noneThrow = std::make_unique<Expr>();
                noneThrow->node = TryThrow{lit(LitKind::Atom, "none")};
                noneClause.body = std::move(noneThrow);
                // Native Optional-returning APIs may erase Just on success
                // and return the payload directly. Static checking guarantees
                // the operand is Optional/Result; preserve that ABI here.
                auto erasedVar = fresh("_try_erased");
                MatchClause erasedSuccessClause;
                erasedSuccessClause.patterns.push_back(mkVarPat(erasedVar));
                erasedSuccessClause.body = var(erasedVar);

                Match m;
                m.subjects.push_back(std::move(sAtom));
                m.clauses.push_back(std::move(okClause));
                m.clauses.push_back(std::move(justClause));
                m.clauses.push_back(std::move(errClause));
                m.clauses.push_back(std::move(noneClause));
                m.clauses.push_back(std::move(erasedSuccessClause));
                auto ex = std::make_unique<Expr>();
                ex->node = std::move(m);
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
                TryCatch tc;
                tc.body = lowerBody(n.body);
                tc.clauses = lowerRescueClauses(n.rescue);
                auto ex = std::make_unique<Expr>();
                ex->node = std::move(tc);
                return ex;
            } else {
                throw LowerError(std::string("IR lower: unimplemented expr node ")
                                 + typeid(T).name());
            }
        }, e->kind);
        currentLoc = prevLoc;
        return r;
    }

    // Operator name (`~(+)`) → the intrinsic op it curries to. Mirrors the
    // string emitter's opBif table (+ and / stay polymorphic via kex_io).
    static auto curryOp(const std::string& name) -> std::optional<Op> {
        static const std::unordered_map<std::string, Op> t = {
            {"+",Op::Add},{"-",Op::Sub},{"*",Op::Mul},{"/",Op::Div},{"%",Op::Mod},{"^",Op::Pow},
            {"==",Op::Eq},{"!=",Op::Neq},{"<",Op::Lt},{"<=",Op::Lte},{">",Op::Gt},{">=",Op::Gte},
            {"&&",Op::And},{"||",Op::Or},
        };
        auto it = t.find(name); return it == t.end() ? std::nullopt : std::optional<Op>(it->second);
    }
    // `name` used as a bare function value, with no arity known statically.
    // Routing through the UFCS method path means a builtin (`digit?`), a
    // module-local function (`stringify`) and a local binding holding a fun
    // all resolve with no special-casing here.
    auto lowerNameAsUfcsFun(const std::string& name,
                            const std::vector<ast::ExprPtr>& args) -> ExprPtr {
        // A LOCAL binding holding a fun IS the fun — pass it through, since
        // the UFCS path would instead look for a method `.name`.
        if (args.empty() && subst.count(name)) return var(currentName(name));

        std::string sx = fresh("Sx");
        auto recvAst = std::make_unique<ast::Expr>();
        recvAst->kind = ast::Identifier{sx};
        ast::MethodCall mc;
        mc.receiver = std::move(recvAst);
        mc.method = name;
        // Borrow the caller's arg exprs for the synthetic call and restore
        // them after — the AST node is shared and re-lowered (multi-clause
        // functions), so it must be left intact.
        auto& lentArgs = const_cast<std::vector<ast::ExprPtr>&>(args);
        mc.args = std::move(lentArgs);
        auto snap = subst; subst[sx] = sx;
        auto body = lowerMethodCall(mc);
        subst = snap;
        lentArgs = std::move(mc.args);
        Lambda lam; lam.params = {sx}; lam.body = std::move(body);
        auto ex = std::make_unique<Expr>(); ex->node = std::move(lam);
        return ex;
    }

    // `~fn(args...)` / `~(op)` — partial or full application. Flatten every
    // paren group into ordered slots (a `_` placeholder is an open slot); if
    // all slots are bound and enough are present, apply now, else build a fun
    // taking one param per open slot plus trailing params up to the arity.
    auto lowerCurry(const ast::CurryExpr& n) -> ExprPtr {
        struct Slot { bool open; ExprPtr val; };
        std::vector<Binding> binds;
        std::vector<Slot> slots;
        for (const auto& g : n.argGroups)
            for (const auto& a : g) {
                if (std::holds_alternative<ast::CurryPlaceholder>(a->kind))
                    slots.push_back({true, nullptr});
                else slots.push_back({false, atomize(a, binds)});
            }
        const std::string qualKey = n.module.empty() ? n.name : n.module + "." + n.name;

        const bool isUnaryOp = n.isOperator && n.name == "!";

        int arity = -1;
        if (n.isOperator) arity = isUnaryOp ? 1 : 2;
        else if (auto it = fnParamNames.find(qualKey); it != fnParamNames.end())
            arity = static_cast<int>(it->second.size());
        else if (!n.module.empty() && externalModules) {
            auto pit = externalModules->exportParamNames.find(qualKey);
            if (pit != externalModules->exportParamNames.end())
                arity = static_cast<int>(pit->second.size());
        }

        // Can this qualified name be emitted as a direct module call?
        const bool qualifiedKnown = !n.module.empty()
            && (moduleFunctions.count(qualKey)
                || (externalModules
                    && externalModules->exportToBeamFn.count(qualKey)
                    && externalModules->nameToAtom.count(n.module)));

        // A bare reference whose arity isn't known here: an unqualified
        // builtin/module-local/local binding, or a qualified name belonging to
        // a prelude module, which is an intrinsic rather than a BEAM module.
        // Emitting Call{name, 0} would be wrong; defer to the UFCS path, where
        // `String.upperCase(s)` and `s.upperCase` are the same call anyway.
        if (!n.isOperator && slots.empty() && arity < 0 && !qualifiedKnown)
            return lowerNameAsUfcsFun(n.name, {});

        auto buildCall = [&](std::vector<ExprPtr> args) -> ExprPtr {
            if (isUnaryOp && args.size() >= 1)
                return intrin(Op::Not, one(std::move(args[0])));
            if (n.isOperator && args.size() >= 2)
                if (auto op = curryOp(n.name))
                    return intrin(*op, two(std::move(args[0]), std::move(args[1])));
            int ar = static_cast<int>(args.size());
            if (!n.module.empty()) {
                // Same two lookups the namespace MethodCall path uses: a
                // function of a module in this compilation unit, else an
                // export of a separately compiled one.
                if (auto it = moduleFunctions.find(qualKey); it != moduleFunctions.end())
                    return callE("", it->second, ar, std::move(args));
                if (externalModules) {
                    auto eit = externalModules->exportToBeamFn.find(qualKey);
                    auto ait = externalModules->nameToAtom.find(n.module);
                    if (eit != externalModules->exportToBeamFn.end()
                        && ait != externalModules->nameToAtom.end())
                        return callE(ait->second, eit->second, ar, std::move(args));
                }
                throw LowerError("IR lower: unknown module function in `~"
                                 + qualKey + "`");
            }
            auto ex = std::make_unique<Expr>();
            ex->node = Call{"", n.name, ar, std::move(args), false};
            return ex;
        };

        int openCount = 0;
        for (const auto& s : slots) if (s.open) openCount++;
        bool full = openCount == 0 &&
            (arity >= 0 ? static_cast<int>(slots.size()) >= arity : !slots.empty());
        if (full) {
            std::vector<ExprPtr> args;
            for (auto& s : slots) args.push_back(std::move(s.val));
            return wrapLets(binds, buildCall(std::move(args)));
        }

        // Partial: a fresh param per open slot, plus trailing params for any
        // arity beyond the slots written (so `~add(1)` on add/2 still takes
        // one more arg).
        int trailing = (arity >= 0) ? std::max(0, arity - static_cast<int>(slots.size())) : 0;
        Lambda lam;
        std::vector<ExprPtr> finalArgs;
        for (auto& s : slots) {
            if (s.open) { std::string p = fresh("P"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
            else finalArgs.push_back(std::move(s.val));
        }
        for (int i = 0; i < trailing; i++) { std::string p = fresh("T"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
        lam.body = buildCall(std::move(finalArgs));
        auto ex = std::make_unique<Expr>(); ex->node = std::move(lam);
        return wrapLets(binds, std::move(ex));
    }

    auto lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr {
        // Supervisor child helpers are syntax-level builders because their
        // blocks become zero-arity start functions rather than ordinary
        // trailing function arguments.
        if (n.name == "worker" && !knownFns.count("worker")) {
            std::vector<Binding> binds;
            ExprPtr startFn;
            if (n.block && n.args.empty() && n.namedArgs.empty()) {
                startFn = atomize(*n.block, binds);
            } else if (!n.args.empty()) {
                auto* module = std::get_if<ast::UpperIdentifier>(&n.args[0]->kind);
                if (module) {
                    std::string beamModule = "kex_";
                    for (char c : module->name)
                        beamModule += static_cast<char>(
                            std::tolower(static_cast<unsigned char>(c)));
                    std::vector<ExprPtr> startArgs;
                    for (const auto& [name, value] : n.namedArgs) {
                        if (name != "args") continue;
                        if (auto* list = std::get_if<ast::ListExpr>(&value->kind))
                            for (const auto& item : list->elements)
                                startArgs.push_back(lower(item));
                    }
                    Lambda start;
                    const int startArity = static_cast<int>(startArgs.size());
                    start.body = callE(beamModule, "start",
                        startArity, std::move(startArgs));
                    auto fn = std::make_unique<Expr>();
                    fn->node = std::move(start);
                    startFn = atomize_ir(std::move(fn), binds);
                }
            }
            if (startFn)
                return wrapLets(binds, callE("kex_supervisor", "worker", 1,
                                             one(std::move(startFn))));
        }
        if (n.name == "supervisor" && n.block && !knownFns.count("supervisor")) {
            std::vector<Binding> binds;
            ExprPtr strategy = lit(LitKind::Atom, "only_crashed");
            for (const auto& [name, value] : n.namedArgs)
                if (name == "strategy" || name == "restart")
                    strategy = lower(value);
            ExprPtr children;
            if (auto* lambda = std::get_if<ast::Lambda>(&(*n.block)->kind))
                children = lowerBody(lambda->body);
            else
                children = lower(*n.block);
            auto pair = [&](const char* key, ExprPtr value) {
                auto tuple = std::make_unique<Expr>();
                tuple->node = MakeTuple{two(lit(LitKind::Atom, key),
                                            std::move(value))};
                return tuple;
            };
            std::vector<ExprPtr> pairs;
            pairs.push_back(pair("strategy", atomize_ir(std::move(strategy), binds)));
            pairs.push_back(pair("children", atomize_ir(std::move(children), binds)));
            auto list = std::make_unique<Expr>();
            list->node = MakeList{std::move(pairs), std::nullopt};
            auto spec = callE("maps", "from_list", 1, one(std::move(list)));
            return wrapLets(binds, callE("kex_supervisor", "start_link", 1,
                                         one(std::move(spec))));
        }
        // Named args → reorder into the callee's positional slots by param
        // name; then positional args (and a trailing block) fill remaining
        // slots in order, leftovers default to None. Mirrors the string
        // emitter / Evaluator::callFunction (spec/optional_parens_do.kex).
        if (!n.namedArgs.empty()) {
            auto it = fnParamNames.find(n.name);
            std::string emittedName = n.name;
            if (it == fnParamNames.end()) {
                for (const auto& [key, val] : moduleFunctions) {
                    auto dot = key.rfind('.');
                    if (dot != std::string::npos && key.substr(dot + 1) == n.name) {
                        auto pit = fnParamNames.find(key);
                        if (pit != fnParamNames.end()) {
                            it = pit;
                            emittedName = val;
                            break;
                        }
                    }
                }
            }
            if (it == fnParamNames.end() && externalModules) {
                for (const auto& [qualKey, pnames] : externalModules->exportParamNames) {
                    auto dot = qualKey.rfind('.');
                    if (dot != std::string::npos && qualKey.substr(dot + 1) == n.name) {
                        auto eit = externalModules->exportToBeamFn.find(qualKey);
                        if (eit != externalModules->exportToBeamFn.end()) {
                            auto modName = qualKey.substr(0, dot);
                            auto ait = externalModules->nameToAtom.find(modName);
                            if (ait != externalModules->nameToAtom.end()) {
                                std::vector<Binding> binds;
                                std::vector<ExprPtr> slots(pnames.size());
                                for (const auto& [an, av] : n.namedArgs)
                                    for (size_t i = 0; i < pnames.size(); i++)
                                        if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
                                std::vector<ExprPtr> positional;
                                for (const auto& a : n.args) positional.push_back(atomize(a, binds));
                                if (n.block) positional.push_back(atomize(*n.block, binds));
                                size_t next = 0;
                                for (auto& p : positional) {
                                    while (next < slots.size() && slots[next]) next++;
                                    if (next >= slots.size()) break;
                                    slots[next] = std::move(p);
                                }
                                for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                                int ar = static_cast<int>(slots.size());
                                return wrapLets(binds,
                                    callE(ait->second, eit->second, ar, std::move(slots)));
                            }
                        }
                    }
                }
            }
            if (it == fnParamNames.end())
                throw LowerError("IR lower: named args to unknown function " + n.name);
            const auto& pnames = it->second;
            std::vector<Binding> binds;
            std::vector<ExprPtr> slots(pnames.size());
            for (const auto& [an, av] : n.namedArgs)
                for (size_t i = 0; i < pnames.size(); i++)
                    if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
            std::vector<ExprPtr> positional;
            for (const auto& a : n.args) positional.push_back(atomize(a, binds));
            if (n.block) positional.push_back(atomize(*n.block, binds));
            size_t next = 0;
            for (auto& p : positional) {
                while (next < slots.size() && slots[next]) next++;
                if (next >= slots.size()) break;
                slots[next] = std::move(p);
            }
            for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
            auto ex = std::make_unique<Expr>();
            int ar = (int)slots.size();
            ex->node = Call{"", emittedName, ar, std::move(slots), false};
            return wrapLets(binds, std::move(ex));
        }
        std::vector<Binding> binds;
        // describe/it: the testing DSL → kex_test, block as a 0-arg fun.
        if ((n.name == "describe" || n.name == "it") && n.block && n.args.size() == 1) {
            auto name = atomize(n.args[0], binds);
            auto fn = atomize(*n.block, binds);
            return wrapLets(binds, callE("kex_test", n.name, 2, two(std::move(name), std::move(fn))));
        }
        // before/after hooks register a 0-arg block in the current describe;
        // an optional :each/:all atom selects its scope.
        if ((n.name == "before" || n.name == "after") && n.block && n.args.size() <= 1) {
            std::vector<ExprPtr> args;
            for (const auto& arg : n.args) args.push_back(atomize(arg, binds));
            auto fn = atomize(*n.block, binds);
            args.push_back(std::move(fn));
            auto arity = static_cast<int>(args.size());
            return wrapLets(binds, callE("kex_test", n.name, arity, std::move(args)));
        }
        // assert(cond[, msg]) — a plain global builtin, not a local function.
        if (n.name == "assert" && !n.args.empty() && !n.block) {
            std::vector<ExprPtr> as;
            for (const auto& a : n.args) as.push_back(atomize(a, binds));
            int ar = static_cast<int>(as.size());
            return wrapLets(binds, callE("kex_test", "assert", ar, std::move(as)));
        }
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        // A trailing do-block is passed as the function's last argument.
        if (n.block) args.push_back(atomize(*n.block, binds));
        if (n.name == "self" && args.empty() && !knownFns.count("self"))
            return callE("erlang", "self", 0, {});
        if (n.name == "send" && args.size() == 2 && !knownFns.count("send")) {
            auto message = std::make_unique<Expr>();
            message->node = MakeTuple{three(lit(LitKind::Atom, "kex_msg"),
                                            std::move(args[1]),
                                            callE("erlang", "self", 0, {}))};
            return wrapLets(binds, callE("erlang", "send", 2,
                                         two(std::move(args[0]),
                                             std::move(message))));
        }
        auto ex = std::make_unique<Expr>();
        int arity = static_cast<int>(args.size());
        // A 0-arity function/constant holding a fun (e.g. `let inc = ~add(1)`)
        // called with args: evaluate the thunk, then apply the resulting fun.
        bool zeroArgThunk = !subst.count(n.name) &&
            (zeroArgFns.count(n.name) || topLevelConstants.count(n.name));
        // Capitalized name = ADT constructor with a payload → tagged tuple.
        if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))
            && !zeroArgThunk) {
            if (auto tag = qualifiedVariantTag(n.name); !tag.empty()) {
                ex->node = Construct{tag, std::move(args)};
                return wrapLets(binds, std::move(ex));
            }
            if (staticCtorOutOfScope(n.name))
                return wrapLets(binds, runtimeError("Undefined function: " + n.name));
            ex->node = Construct{n.name, std::move(args)};
        }
        else if (zeroArgThunk && (!args.empty() || topLevelConstants.count(n.name))) {
            // A `let` constant holding a fun: `name(...)` resolves the
            // constant, then applies — including `thunk()` with NO args (the
            // walker looks the identifier up and applies the lambda once).
            // A real 0-arity FUNCTION `f()` stays a plain call below: its
            // result is returned as-is, never auto-applied.
            auto thunk = std::make_unique<Expr>();
            thunk->node = Call{"", n.name, 0, {}, false};
            ex->node = CallIndirect{std::move(thunk), std::move(args), false};
        } else if (zeroArgThunk)
            ex->node = Call{"", n.name, 0, {}, false};
        else if (knownFns.count(n.name))
            ex->node = Call{"", n.name, arity, std::move(args), false};
        else if (auto imp = moduleImports.find(n.name); imp != moduleImports.end())
            ex->node = Call{"", imp->second, arity, std::move(args), false};
        else if (subst.count(n.name))
            // A lexical binding (for example a `block` parameter) can hold a
            // callable value. Keep this indirect apply distinct from a truly
            // unknown free function, which must fail only if executed.
            ex->node = CallIndirect{var(currentName(n.name)), std::move(args), false};
        else
            return wrapLets(binds, runtimeError("Undefined function: " + n.name));
        return wrapLets(binds, std::move(ex));
    }

    // Receiver-call and stdlib compatibility resolution. Unknown forms fail
    // explicitly rather than silently emitting a nonexistent function.
    // Set while lowering a mutating `!` call's VALUE (the rebind itself is
    // handled by the enclosing statement — see lowerBodyFrom/loop handling).
    bool m_lowerMutatingAsValue = false;
    // A module path `A.B.C` (nested modules, encoded by the parser as a chain
    // of MethodCall nodes with no args) flattened to the qualified name
    // ["A","B","C"], or empty if the receiver isn't a pure uppercase path.
    static auto modulePath(const ast::Expr& e, std::vector<std::string>& out) -> bool {
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&e.kind)) {
            out.push_back(uid->name); return true;
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&e.kind)) {
            if (!mc->args.empty() || !mc->namedArgs.empty() || mc->block) return false;
            if (!mc->method.empty() && std::isupper(static_cast<unsigned char>(mc->method[0]))) {
                if (!modulePath(*mc->receiver, out)) return false;
                out.push_back(mc->method); return true;
            }
        }
        return false;
    }

    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
        // A bare mutating `!` call used where its rebind can't be applied is
        // a runtime error (matching the walker's behaviour). Statement-position
        // `!` calls are lowered by the caller as value + rebind.
        if (n.mutating && !m_lowerMutatingAsValue) {
            std::string loc;
            if (currentLoc) {
                loc = std::string(currentLoc->file) + ":" + std::to_string(currentLoc->line) + ":"
                    + std::to_string(currentLoc->column) + ": ";
            }
            return callE("erlang", "error", 1, one(
                lit(LitKind::String, loc + "runtime error: '!' requires a variable binding as the receiver")));
        }
        const bool thisReceiver =
            std::holds_alternative<ast::ThisExpr>(n.receiver->kind) ||
            (std::holds_alternative<ast::Identifier>(n.receiver->kind) &&
             std::get<ast::Identifier>(n.receiver->kind).name == "this");
        if (thisReceiver && n.args.empty() && n.namedArgs.empty() &&
            !n.block && fieldAccessors.count(n.method)) {
            const auto& candidates = fieldAccessors.at(n.method);
            auto selected = std::find_if(
                candidates.begin(), candidates.end(),
                [&](const auto& candidate) {
                    return candidate.first == currentMakeType;
                });
            if (selected != candidates.end())
                return callE("erlang", "element", 2,
                    two(litInt(selected->second), lower(n.receiver)));
        }
        // `Type.of(x)` the checker answered: emit the recorded shape as a
        // literal record. Ahead of every dispatch path, like the walker's.
        if (staticTypeOfCalls) {
            auto recorded = staticTypeOfCalls->find(&n);
            if (recorded != staticTypeOfCalls->end()) {
                std::vector<Binding> binds;
                // A value argument still runs (it may have effects); one that
                // NAMES a function does not — `Date.parse` alone is a call
                // missing its argument, which would not even compile.
                if (recorded->second.evaluateArgument)
                    for (const auto& arg : n.args)
                        if (arg) binds.push_back({fresh("TypeArg"), lower(arg)});
                return wrapLets(binds, structuredTypeLiteral(recorded->second.type));
            }
        }
        if (resolvedCalls) {
            // Local types shadow prelude-resolved calls (a user `record Parser`
            // must win over the prelude `module Parser`).
            bool localTypeShadows = false;
            if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind))
                localTypeShadows = knownTypes.count(uid->name) && localMethods.count(n.method);
            if (auto* id = std::get_if<ast::Identifier>(&n.receiver->kind))
                localTypeShadows = id->name == "this";
            if (std::holds_alternative<ast::ThisExpr>(n.receiver->kind))
                localTypeShadows = true;
            auto resolved = resolvedCalls->find(&n);
            // Semantic analysis has already applied lexical `using` policy
            // and argument-type specificity. Once it names an imported
            // target, source-import priority must not override that choice.
            // Prelude self-compilation is the one exception: its analyzer is
            // intentionally backed by the previous prebuilt interface while
            // lowering the replacement artifact, so its source imports are
            // newer than those resolved targets.
            bool stalePreludeTarget = preferExternalReceivers &&
                !std::holds_alternative<ast::UpperIdentifier>(
                    n.receiver->kind) &&
                moduleImports.count(n.method);
            // A bare `x.field` whose name is also an imported receiver method
            // (`name` is Weekday's and Type's) must go through THIS module's
            // accessor, which matches the record tags and delegates to the
            // import for anything else. Calling the import directly skips the
            // record arms, so an erased receiver — `opaque(p).name`, or any
            // REPL value — died with "Undefined method: name for Person"
            // while a statically typed `p.name` read the field fine.
            const bool localFieldAccessor =
                n.args.empty() && n.namedArgs.empty() && !n.block &&
                !n.mutating && fieldAccessors.count(n.method);
            if (resolved != resolvedCalls->end() && !localTypeShadows &&
                !stalePreludeTarget && !localFieldAccessor) {
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                if (resolved->second.passesReceiver)
                    args.push_back(atomize(n.receiver, binds));
                if (!n.namedArgs.empty()) {
                    const auto& pnames = resolved->second.paramNames;
                    auto expected = static_cast<size_t>(
                        resolved->second.backendArity -
                        (resolved->second.passesReceiver ? 1 : 0));
                    if (pnames.size() != expected)
                        throw LowerError(
                            "IR lower: named args to imported function with unknown params: " +
                            resolved->second.backendFunction);
                    std::vector<ExprPtr> slots(pnames.size());
                    for (const auto& [name, value] : n.namedArgs) {
                        auto it = std::find(pnames.begin(), pnames.end(), name);
                        if (it == pnames.end())
                            throw LowerError("IR lower: unknown named arg " + name +
                                             " for " + resolved->second.backendFunction);
                        slots[static_cast<size_t>(it - pnames.begin())] =
                            atomize(value, binds);
                    }
                    std::vector<ExprPtr> positional;
                    for (const auto& arg : n.args)
                        positional.push_back(atomize(arg, binds));
                    if (n.block) positional.push_back(atomize(*n.block, binds));
                    size_t next = 0;
                    for (auto& value : positional) {
                        while (next < slots.size() && slots[next]) next++;
                        if (next >= slots.size()) break;
                        slots[next++] = std::move(value);
                    }
                    for (auto& slot : slots)
                        if (!slot) slot = lit(LitKind::None, "none");
                    for (auto& slot : slots) args.push_back(std::move(slot));
                } else {
                    for (const auto& arg : n.args)
                        args.push_back(atomize(arg, binds));
                    if (n.block) args.push_back(atomize(*n.block, binds));
                }
                auto backendFunction = resolved->second.backendFunction;
                if (!resolved->second.localDispatchTypes.empty()) {
                    auto exact = mangleReceiverSignature(
                        backendFunction,
                        resolved->second.localDispatchTypes);
                    auto byReceiver = mangleReceiverImplementation(
                        backendFunction,
                        resolved->second.localDispatchTypes.front());
                    if (knownFns.count(exact))
                        backendFunction = std::move(exact);
                    else if (knownFns.count(byReceiver))
                        backendFunction = std::move(byReceiver);
                }
                return wrapLets(
                    binds,
                    callE(resolved->second.backendModule,
                          backendFunction,
                          resolved->second.backendArity,
                          std::move(args)));
            }
        }
        // A call into the `Kex.Intrinsic.<Category>` runtime module, e.g.
        // `Kex.Intrinsic.List.reverse(x)`. Compile to a plain cross-module call
        // `call 'kex_intrinsic_list':'reverse'(x)` — the emitter knows NOTHING
        // about `reverse`; the runtime module owns it, and the Kex prelude above
        // owns the typed semantics. Adding a primitive = add a runtime function,
        // with zero emitter changes.
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && path.size() >= 3 &&
                path[0] == "Kex" && path[1] == "Intrinsic") {
                std::string mod = "kex_intrinsic";
                for (size_t i = 2; i < path.size(); i++) {
                    mod += "_";
                    for (char c : path[i]) mod += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                auto ex = std::make_unique<Expr>();
                int ar = static_cast<int>(args.size());
                ex->node = Call{mod, n.method, ar, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
        // Erlang.*/Elixir.*/Gleam.* interop: direct BEAM module calls.
        // `Erlang.lists.reverse(xs)` → `call 'lists':'reverse'(xs)`
        // `Elixir.Phoenix.Router.match(c)` → `call 'Elixir.Phoenix.Router':'match'(c)`
        // `Gleam.wisp.serve(h)` → `call 'wisp':'serve'(h)`
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && !path.empty() &&
                (path[0] == "Erlang" || path[0] == "Elixir" || path[0] == "Gleam")) {
                std::string mod;
                if (path[0] == "Elixir") {
                    for (size_t i = 1; i < path.size(); i++) {
                        if (i > 1) mod += ".";
                        mod += path[i];
                    }
                    mod = "Elixir." + mod;
                } else {
                    // Erlang/Gleam: lowercase all segments
                    for (size_t i = 1; i < path.size(); i++) {
                        if (i > 1) mod += ".";
                        std::string seg = path[i];
                        for (auto& c : seg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        mod += seg;
                    }
                }
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                int ar = static_cast<int>(args.size());
                auto ex = std::make_unique<Expr>();
                ex->node = Call{mod, n.method, ar, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
        // User module function: `Util.double(21)` / `Util.Math.double(21)`.
        // Resolve first against the explicit receiver path, then relative to
        // the enclosing module path so nested modules can refer to siblings.
        {
            std::vector<std::string> path;
            if (!path.empty() || modulePath(*n.receiver, path)) {
                auto pathToString = [&](const std::vector<std::string>& p) {
                    std::string out;
                    for (size_t i = 0; i < p.size(); i++) {
                        if (i) out += ".";
                        out += p[i];
                    }
                    return out;
                };
                std::vector<std::string> candidates;
                auto explicitPath = pathToString(path);
                if (!path.empty()) {
                    if (auto alias = moduleAliases.find(path.front());
                        alias != moduleAliases.end()) {
                        explicitPath = alias->second;
                        for (size_t i = 1; i < path.size(); ++i)
                            explicitPath += "." + path[i];
                    }
                }
                candidates.push_back(explicitPath);
                if (!currentModulePath.empty() && path.size() == 1) {
                    std::string relative = currentModulePath;
                    relative += "." + pathToString(path);
                    candidates.push_back(std::move(relative));
                }
                std::unordered_set<std::string> tried;
                for (const auto& candidate : candidates) {
                    if (candidate.empty() || !tried.insert(candidate).second) continue;
                    auto qualKey = candidate + "." + n.method;
                    auto it = moduleFunctions.find(qualKey);
                    if (it != moduleFunctions.end()) {
                        std::vector<Binding> binds;
                        if (!n.namedArgs.empty()) {
                            auto pit = fnParamNames.find(qualKey);
                            if (pit == fnParamNames.end())
                                throw LowerError("IR lower: named args to module function with unknown params: " + qualKey);
                            const auto& pnames = pit->second;
                            std::vector<ExprPtr> slots(pnames.size());
                            for (const auto& [an, av] : n.namedArgs)
                                for (size_t i = 0; i < pnames.size(); i++)
                                    if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
                            std::vector<ExprPtr> positional;
                            for (const auto& a : n.args) positional.push_back(atomize(a, binds));
                            if (n.block) positional.push_back(atomize(*n.block, binds));
                            size_t next = 0;
                            for (auto& p : positional) {
                                while (next < slots.size() && slots[next]) next++;
                                if (next >= slots.size()) break;
                                slots[next] = std::move(p);
                            }
                            for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                            int ar = static_cast<int>(slots.size());
                            return wrapLets(binds, callE("", it->second, ar, std::move(slots)));
                        }
                        std::vector<ExprPtr> args;
                        for (const auto& a : n.args) args.push_back(atomize(a, binds));
                        if (n.block) args.push_back(atomize(*n.block, binds));
                        int ar = static_cast<int>(args.size());
                        return wrapLets(binds, callE("", it->second, ar, std::move(args)));
                    }
                }
                // External loaded modules: BinaryTree.fromList → 'Kex.BinaryTree':fromList
                // Skip when the receiver names a local type that owns this
                // method — local types shadow prelude modules with the same name.
                if (externalModules
                    && !(path.size() == 1 && knownTypes.count(path[0])
                         && localMethods.count(n.method))) {
                    for (const auto& candidate : candidates) {
                        if (candidate.empty()) continue;
                        auto qualKey = candidate + "." + n.method;
                        auto eit = externalModules->exportToBeamFn.find(qualKey);
                        if (eit != externalModules->exportToBeamFn.end()) {
                            auto ait = externalModules->nameToAtom.find(candidate);
                            if (ait != externalModules->nameToAtom.end()) {
                                std::vector<Binding> binds;
                                if (!n.namedArgs.empty()) {
                                    auto pit = externalModules->exportParamNames.find(qualKey);
                                    if (pit == externalModules->exportParamNames.end())
                                        throw LowerError("IR lower: named args to external function with unknown params: " + qualKey);
                                    const auto& pnames = pit->second;
                                    std::vector<ExprPtr> slots(pnames.size());
                                    for (const auto& [an, av] : n.namedArgs)
                                        for (size_t i = 0; i < pnames.size(); i++)
                                            if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
                                    std::vector<ExprPtr> positional;
                                    for (const auto& a : n.args) positional.push_back(atomize(a, binds));
                                    if (n.block) positional.push_back(atomize(*n.block, binds));
                                    size_t next = 0;
                                    for (auto& p : positional) {
                                        while (next < slots.size() && slots[next]) next++;
                                        if (next >= slots.size()) break;
                                        slots[next] = std::move(p);
                                    }
                                    for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                                    int ar = static_cast<int>(slots.size());
                                    return wrapLets(binds,
                                        callE(ait->second, eit->second, ar, std::move(slots)));
                                }
                                std::vector<ExprPtr> args;
                                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                                if (n.block) args.push_back(atomize(*n.block, binds));
                                int ar = static_cast<int>(args.size());
                                return wrapLets(binds,
                                    callE(ait->second, eit->second, ar, std::move(args)));
                            }
                        }
                    }
                }
                if (auto rit = records.find(n.method); rit != records.end()) {
                    std::vector<Binding> binds;
                    const auto& info = rit->second;
                    std::unordered_map<std::string, const ast::ExprPtr*> provided;
                    for (const auto& [name, val] : n.namedArgs)
                        provided[name] = &val;
                    if (n.block) {
                        auto extractMap = [&](const ast::MapExpr* map) {
                            for (const auto& entry : map->entries)
                                if (auto* atom = std::get_if<ast::AtomLiteral>(&entry.key->kind))
                                    provided[atom->name] = &entry.value;
                        };
                        if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind)) {
                            if (!lam->body.empty())
                                if (auto* map = std::get_if<ast::MapExpr>(&lam->body.back()->kind))
                                    extractMap(map);
                        } else if (auto* map = std::get_if<ast::MapExpr>(&(*n.block)->kind)) {
                            extractMap(map);
                        }
                    }
                    std::vector<ExprPtr> fieldArgs;
                    for (size_t i = 0; i < info.fields.size(); i++) {
                        auto pit = provided.find(info.fields[i]);
                        if (pit != provided.end() && *pit->second)
                            fieldArgs.push_back(lower(*pit->second));
                        else if (info.defaults[i])
                            fieldArgs.push_back(lower(*info.defaults[i]));
                        else
                            fieldArgs.push_back(lit(LitKind::None, "none"));
                    }
                    auto ex = std::make_unique<Expr>();
                    ex->node = Construct{n.method, std::move(fieldArgs)};
                    return wrapLets(binds, std::move(ex));
                }
            }
        }
        // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            std::vector<Binding> binds;
            if (auto it = moduleFunctions.find(uid->name + "." + n.method);
                it != moduleFunctions.end()) {
                auto qualKey = uid->name + "." + n.method;
                if (!n.namedArgs.empty()) {
                    auto pit = fnParamNames.find(qualKey);
                    if (pit == fnParamNames.end())
                        throw LowerError("IR lower: named args to module function with unknown params: " + qualKey);
                    const auto& pnames = pit->second;
                    std::vector<ExprPtr> slots(pnames.size());
                    for (const auto& [an, av] : n.namedArgs)
                        for (size_t i = 0; i < pnames.size(); i++)
                            if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
                    std::vector<ExprPtr> positional;
                    for (const auto& a : n.args) positional.push_back(atomize(a, binds));
                    if (n.block) positional.push_back(atomize(*n.block, binds));
                    size_t next = 0;
                    for (auto& p : positional) {
                        while (next < slots.size() && slots[next]) next++;
                        if (next >= slots.size()) break;
                        slots[next] = std::move(p);
                    }
                    for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                    int ar = static_cast<int>(slots.size());
                    return wrapLets(binds, callE("", it->second, ar, std::move(slots)));
                }
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                int ar = static_cast<int>(args.size());
                return wrapLets(binds, callE("", it->second, ar, std::move(args)));
            }
            // External module dispatch — but skip when the receiver names a
            // local type that owns this method (local types shadow prelude
            // modules with the same name, e.g. a user `record Parser` shadows
            // the prelude's `module Parser`).
            if (externalModules
                && !(knownTypes.count(uid->name) && localMethods.count(n.method))) {
                auto qualKey = uid->name + "." + n.method;
                auto eit = externalModules->exportToBeamFn.find(qualKey);
                if (eit != externalModules->exportToBeamFn.end()) {
                    auto ait = externalModules->nameToAtom.find(uid->name);
                    if (ait != externalModules->nameToAtom.end()) {
                        if (!n.namedArgs.empty()) {
                            auto pit = externalModules->exportParamNames.find(qualKey);
                            if (pit == externalModules->exportParamNames.end())
                                throw LowerError("IR lower: named args to external function with unknown params: " + qualKey);
                            const auto& pnames = pit->second;
                            std::vector<ExprPtr> slots(pnames.size());
                            for (const auto& [an, av] : n.namedArgs)
                                for (size_t i = 0; i < pnames.size(); i++)
                                    if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
                            std::vector<ExprPtr> positional;
                            for (const auto& a : n.args) positional.push_back(atomize(a, binds));
                            if (n.block) positional.push_back(atomize(*n.block, binds));
                            size_t next = 0;
                            for (auto& p : positional) {
                                while (next < slots.size() && slots[next]) next++;
                                if (next >= slots.size()) break;
                                slots[next] = std::move(p);
                            }
                            for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                            int ar = static_cast<int>(slots.size());
                            return wrapLets(binds,
                                callE(ait->second, eit->second, ar, std::move(slots)));
                        }
                        std::vector<ExprPtr> args;
                        for (const auto& a : n.args) args.push_back(atomize(a, binds));
                        if (n.block) args.push_back(atomize(*n.block, binds));
                        int ar = static_cast<int>(args.size());
                        return wrapLets(binds,
                            callE(ait->second, eit->second, ar, std::move(args)));
                    }
                }
            }
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            // Supervisor.start(restart: strat) do children end →
            // kex_supervisor:start_link(#{strategy => strat, children => Kids}).
            if (uid->name == "Supervisor" && n.method == "start" && n.block) {
                ExprPtr strat = lit(LitKind::Atom, "only_crashed");
                for (const auto& [k, v] : n.namedArgs)
                    if (k == "restart" || k == "strategy") strat = lower(v);
                ExprPtr children;
                if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind))
                    children = lowerBody(lam->body);
                else
                    children = lower(*n.block);
                auto stratA = atomize_ir(std::move(strat), binds);
                auto childA = atomize_ir(std::move(children), binds);
                auto pair = [&](const char* key, ExprPtr val) {
                    auto t = std::make_unique<Expr>();
                    t->node = MakeTuple{two(lit(LitKind::Atom, key), std::move(val))};
                    return t;
                };
                std::vector<ExprPtr> pairs;
                pairs.push_back(pair("strategy", std::move(stratA)));
                pairs.push_back(pair("children", std::move(childA)));
                auto lst = std::make_unique<Expr>();
                lst->node = MakeList{std::move(pairs), std::nullopt};
                auto map = callE("maps", "from_list", 1, one(std::move(lst)));
                return wrapLets(binds, callE("kex_supervisor", "start_link", 1, one(std::move(map))));
            }
            // Static method dispatch: `Type.method(args)` on a user type whose
            // `method` is a local function/make-method. If that method has an
            // implicit `this`, pass a placeholder receiver (the type tag).
            if (knownTypes.count(uid->name) && localMethods.count(n.method)) {
                std::string mangled =
                    mangleQualifiedMember(uid->name, n.method);
                std::string callName = knownFns.count(mangled) ? mangled : n.method;
                std::vector<ExprPtr> callArgs;
                if (implicitThisMethods.count(callName))
                    callArgs.push_back(lit(LitKind::Atom, uid->name));
                for (auto& a : args) callArgs.push_back(std::move(a));
                int ar = static_cast<int>(callArgs.size());
                auto ex = std::make_unique<Expr>();
                ex->node = Call{"", callName, ar, std::move(callArgs), false};
                return wrapLets(binds, std::move(ex));
            }
            if (auto rit = records.find(n.method); rit != records.end()) {
                const auto& info = rit->second;
                std::unordered_map<std::string, const ast::ExprPtr*> provided;
                for (const auto& [name, val] : n.namedArgs)
                    provided[name] = &val;
                if (n.block) {
                    auto extractMap = [&](const ast::MapExpr* map) {
                        for (const auto& entry : map->entries)
                            if (auto* atom = std::get_if<ast::AtomLiteral>(&entry.key->kind))
                                provided[atom->name] = &entry.value;
                    };
                    if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind)) {
                        if (!lam->body.empty())
                            if (auto* map = std::get_if<ast::MapExpr>(&lam->body.back()->kind))
                                extractMap(map);
                    } else if (auto* map = std::get_if<ast::MapExpr>(&(*n.block)->kind)) {
                        extractMap(map);
                    }
                }
                std::vector<ExprPtr> fieldArgs;
                for (size_t i = 0; i < info.fields.size(); i++) {
                    auto pit = provided.find(info.fields[i]);
                    if (pit != provided.end() && *pit->second)
                        fieldArgs.push_back(lower(*pit->second));
                    else if (info.defaults[i])
                        fieldArgs.push_back(lower(*info.defaults[i]));
                    else
                        fieldArgs.push_back(lit(LitKind::None, "none"));
                }
                auto ex = std::make_unique<Expr>();
                ex->node = Construct{n.method, std::move(fieldArgs)};
                return wrapLets(binds, std::move(ex));
            }
            // `Mod.Tag` for an ADT declared inside a module (e.g. `Kex.FS`).
            // The tag is just an atom at runtime, so the qualifier has no
            // representation here — emit the bare tag.
            if (n.args.empty() && !n.block && n.namedArgs.empty()) {
                if (auto tag = qualifiedVariantTag(uid->name + "." + n.method);
                    !tag.empty()) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Construct{tag, {}};
                    return wrapLets(binds, std::move(ex));
                }
            }
            // A nullary CONSTRUCTOR is a value, not a namespace: `Monday.name`
            // is the same call as `let m = Monday` then `m.name`, which has
            // always worked. Fall through to the value-receiver path below
            // rather than reporting the namespace as undefined.
            const bool nullaryConstructorReceiver =
                nullaryVariantTags.count(uid->name) ||
                // Prelude/imported constructors are not in the local tag set;
                // the external variant table (seeded for display) knows them.
                (variantOwner.count(uid->name) &&
                 variantArity.count(uid->name) &&
                 variantArity.at(uid->name) == 0);
            if (!nullaryConstructorReceiver)
                return wrapLets(binds,
                    runtimeError("Undefined function: " + uid->name + "." + n.method));
        }
        // UFCS method on a value receiver. Atomize the receiver once so
        // methods that use it several times (min/get/count/…) don't
        // re-evaluate it; `ret` wraps the result in that binding.
        std::vector<Binding> rb;
        auto rv_ = atomize_ir(lower(n.receiver), rb);
        // Extract the atomic name so we can produce fresh Var refs on demand
        // without relying on cloning the original unique_ptr (avoids a
        // GCC-specific null-after-move issue on Linux CI).
        auto rvName = std::get_if<Var>(&rv_->node)
            ? std::get_if<Var>(&rv_->node)->name : std::string{};
        auto rvLit = std::get_if<Lit>(&rv_->node)
            ? std::optional<Lit>{*std::get_if<Lit>(&rv_->node)} : std::nullopt;
        auto rv = [&]() -> ExprPtr {
            if (!rvName.empty()) return var(rvName);
            if (rvLit) { auto e = std::make_unique<Expr>(); e->node = *rvLit; return e; }
            return var("_rv_bug");
        };
        auto& m = n.method;
        auto ret = [&](ExprPtr e) { return wrapLets(rb, std::move(e)); };
        auto arg0 = [&]() { return lower(n.args[0]); };

        // A public function brought into scope by `using Module` also
        // participates in UFCS: `100.meter` is `meter(100)`, and
        // `distance.per(duration)` is `per(distance, duration)`. The source
        // import must win over an identically named prelude receiver method:
        // module compilation merges declarations into one flat IR first, so
        // imported functions also appear in localMethods at this point.
        if (auto imported = moduleImports.find(m);
            imported != moduleImports.end()) {
                const std::vector<std::string>* pnames = nullptr;
                for (const auto& [qualified, emitted] : moduleFunctions) {
                    if (emitted != imported->second) continue;
                    if (auto names = fnParamNames.find(qualified);
                        names != fnParamNames.end())
                        pnames = &names->second;
                    break;
                }

                std::vector<ExprPtr> args;
                if (!n.namedArgs.empty() && pnames) {
                    std::vector<ExprPtr> slots(pnames->size());
                    if (!slots.empty()) slots[0] = rv();
                    for (const auto& [name, value] : n.namedArgs) {
                        auto it = std::find(pnames->begin(), pnames->end(), name);
                        if (it == pnames->end() || it == pnames->begin())
                            throw LowerError("IR lower: unknown named arg " + name +
                                             " for imported UFCS function " + m);
                        slots[static_cast<size_t>(it - pnames->begin())] =
                            atomize_ir(lower(value), rb);
                    }
                    size_t next = 1;
                    for (const auto& value : n.args) {
                        while (next < slots.size() && slots[next]) next++;
                        if (next >= slots.size()) break;
                        slots[next++] = atomize_ir(lower(value), rb);
                    }
                    if (n.block) {
                        while (next < slots.size() && slots[next]) next++;
                        if (next < slots.size())
                            slots[next] = atomize_ir(lower(*n.block), rb);
                    }
                    for (auto& slot : slots)
                        if (!slot) slot = lit(LitKind::None, "none");
                    args = std::move(slots);
                } else {
                    args.push_back(rv());
                    for (const auto& value : n.args)
                        args.push_back(atomize_ir(lower(value), rb));
                    for (const auto& [_, value] : n.namedArgs)
                        args.push_back(atomize_ir(lower(value), rb));
                    if (n.block)
                        args.push_back(atomize_ir(lower(*n.block), rb));
                }
            const int arity = static_cast<int>(args.size());
            return ret(callE("", imported->second, arity, std::move(args)));
        }

        // A local method only claims the call when it is defined at THIS
        // arity; otherwise the external/prelude receiver function handles it.
        // `this.map.get(k, default)` inside a `make Box do get(key) ... end`
        // block used to compile to a local get/3 that was never defined.
        const int ufcsArity =
            static_cast<int>(n.args.size() + n.namedArgs.size()) + 1 +
            (n.block ? 1 : 0);
        const bool localArityExists =
            localMethodArities.empty() ||
            localMethodArities.count(m + "/" + std::to_string(ufcsArity)) > 0;

        // External receiver functions take priority over prelude for UFCS.
        // The registry includes only package-declared provider modules here;
        // ordinary module exports never become receiver functions implicitly.
        if (externalModules
            && (!localMethods.count(m) || !localArityExists
                || preferExternalReceivers)) {
            auto found = externalModules->receiverFunctions.find(m);
            if (found != externalModules->receiverFunctions.end()) {
                int actualArity = static_cast<int>(n.args.size() + n.namedArgs.size()) + 1 +
                                  (n.block ? 1 : 0);
                const ExternalModules::ReceiverFunction* match = nullptr;
                for (const auto& candidate : found->second) {
                    if (candidate.beamArity != actualArity) continue;
                    if (match)
                        return ret(runtimeError(
                            "Ambiguous receiver function: " + m + "/" +
                            std::to_string(actualArity)));
                    match = &candidate;
                }
                if (match) {
                    std::vector<ExprPtr> pargs;
                    pargs.push_back(rv());
                    if (!n.namedArgs.empty()) {
                        auto expected = static_cast<size_t>(actualArity - 1);
                        if (match->paramNames.size() != expected)
                            throw LowerError(
                                "IR lower: named args to receiver function with unknown params: " + m);
                        std::vector<ExprPtr> slots(match->paramNames.size());
                        for (const auto& [name, value] : n.namedArgs) {
                            auto it = std::find(match->paramNames.begin(),
                                                match->paramNames.end(), name);
                            if (it == match->paramNames.end())
                                throw LowerError("IR lower: unknown named arg " + name +
                                                 " for receiver function " + m);
                            slots[static_cast<size_t>(it - match->paramNames.begin())] =
                                atomize_ir(lower(value), rb);
                        }
                        std::vector<ExprPtr> positional;
                        for (const auto& a : n.args)
                            positional.push_back(atomize_ir(lower(a), rb));
                        if (n.block)
                            positional.push_back(atomize_ir(lower(*n.block), rb));
                        size_t next = 0;
                        for (auto& value : positional) {
                            while (next < slots.size() && slots[next]) next++;
                            if (next >= slots.size()) break;
                            slots[next++] = std::move(value);
                        }
                        for (auto& slot : slots)
                            if (!slot) slot = lit(LitKind::None, "none");
                        for (auto& slot : slots) pargs.push_back(std::move(slot));
                    } else {
                        for (const auto& a : n.args)
                            pargs.push_back(atomize_ir(lower(a), rb));
                        if (n.block)
                            pargs.push_back(atomize_ir(lower(*n.block), rb));
                    }
                    return ret(callE(match->moduleAtom, match->beamFunction,
                                     actualArity, std::move(pargs)));
                }
            }
        }
        // Generic UFCS fallback: a field access or a make-block method are
        // BOTH emitted as local functions taking the receiver first, so
        // `x.foo(a, b)` → `apply 'foo'/3(x, a, b)`. This is exactly how the
        // string emitter resolves record field access and make-block methods
        // without needing types. Gated on localMethods so an UNPORTED builtin
        // (e.g. `.get`/`.map`) errors loudly instead of silently becoming a
        // call to a function that doesn't exist. A trailing block (`x.some { ...
        // }`) is passed as the final argument — the block-typed parameter — to
        // match the interpreter, which appends the evaluated block to the args.
        // Only take the local path when a local definition exists at THIS
        // arity. `this.map.get(k, default)` inside a `make Box do get(key) ...`
        // block otherwise compiled to a local get/3 that was never defined,
        // because the name alone matched. Falling through lets the prelude's
        // receiver function (Map.get here) handle it, which is what the
        // interpreter already does.
        if (localMethods.count(n.method) && localArityExists) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            args.push_back(rv());
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            // Make-block methods lower receiver-first. Named arguments follow
            // positional ones in the common `method(Type, name: value)` form.
            for (const auto& [_, value] : n.namedArgs)
                args.push_back(atomize(value, binds));
            if (n.block) {
                args.push_back(atomize(*n.block, binds));
            } else if (auto it = methodDefaults.find(n.method); it != methodDefaults.end()) {
                for (size_t i = n.args.size(); i < it->second.size(); i++)
                    if (it->second[i]) args.push_back(atomize(*it->second[i], binds));
            }
            int arity = static_cast<int>(args.size());
            auto ex = std::make_unique<Expr>();
            ex->node = Call{"", n.method, arity, std::move(args), false};
            return ret(wrapLets(binds, std::move(ex)));
        }
        // External loaded module methods (UFCS): tree.size → 'Kex.BinaryTree':'Tree.size'(tree)
        return ret(runtimeError("Undefined method: " + n.method));
    }

    // record TypeName { f: v, ... } → {'TypeName', <fields in declared order,
    // defaults filled for omitted ones>}. Field ORDER and defaults come from
    // the record definition (the accessors read fixed positions).
    auto lowerRecordConstruction(const ast::RecordConstruction& n) -> ExprPtr {
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        auto it = records.find(n.typeName);
        if (it == records.end()) {
            // Unknown record — fall back to fields as written.
            for (const auto& [name, v] : n.fields) args.push_back(atomize(v, binds));
        } else {
            const auto& info = it->second;
            for (size_t i = 0; i < info.fields.size(); i++) {
                const ast::ExprPtr* provided = nullptr;
                for (const auto& [name, v] : n.fields)
                    if (name == info.fields[i]) { provided = &v; break; }
                if (provided) args.push_back(atomize(*provided, binds));
                else if (info.defaults[i]) args.push_back(atomize(*info.defaults[i], binds));
                else args.push_back(lit(LitKind::None, "none"));
            }
        }
        auto ex = std::make_unique<Expr>();
        ex->node = Construct{n.typeName, std::move(args)};
        return wrapLets(binds, std::move(ex));
    }

    // "text ${expr} more" → a ++ chain of literal segments and to_string'd
    // sub-expressions. The `${...}` sub-expressions are raw text in the AST,
    // so they're re-lexed/parsed here and lowered like any other expression
    // This keeps interpolation evaluation order explicit in IR.
    auto lowerInterpolatedString(const std::string& raw) -> ExprPtr {
        std::vector<ExprPtr> parts;
        size_t pos = 0;
        while (pos < raw.size()) {
            auto dollar = raw.find("${", pos);
            if (dollar == std::string::npos) {
                if (pos < raw.size()) parts.push_back(lit(LitKind::String, raw.substr(pos)));
                break;
            }
            if (dollar > pos) parts.push_back(lit(LitKind::String, raw.substr(pos, dollar - pos)));
            size_t close = std::string::npos;
            int depth = 1;
            for (size_t k = dollar + 2; k < raw.size(); k++) {
                if (raw[k] == '{') depth++;
                else if (raw[k] == '}' && --depth == 0) { close = k; break; }
            }
            if (close == std::string::npos) break; // malformed → stop
            std::string inner = raw.substr(dollar + 2, close - dollar - 2);
            kex::Lexer lx(inner);
            kex::Parser ps(lx.tokenizeAll());
            auto innerAst = ps.parseExpr();
            if (innerAst)
                parts.push_back(callE("kex_io", "to_string", 1, one(lower(innerAst))));
            pos = close + 1;
        }
        if (parts.empty()) return lit(LitKind::String, "");
        // A single literal segment IS the string; otherwise build an iolist
        // (binary literals + to_string charlists both qualify) and flatten it
        // to the UTF-8 binary that a Kex String is on BEAM.
        if (parts.size() == 1 && std::holds_alternative<Lit>(parts[0]->node))
            return std::move(parts[0]);
        auto lst = std::make_unique<Expr>();
        lst->node = MakeList{std::move(parts), std::nullopt};
        return callE("unicode", "characters_to_binary", 1, one(std::move(lst)));
    }

    auto lowerParsedInterpolatedString(
        const std::vector<std::string>& stringParts,
        const std::vector<ast::ExprPtr>& values) -> ExprPtr {
        std::vector<ExprPtr> parts;
        for (size_t i = 0; i < stringParts.size(); i++) {
            if (!stringParts[i].empty())
                parts.push_back(lit(LitKind::String, stringParts[i]));
            if (i < values.size() && values[i])
                parts.push_back(
                    callE("kex_io", "to_string", 1, one(lower(values[i]))));
        }
        if (parts.empty()) return lit(LitKind::String, "");
        if (parts.size() == 1 &&
            std::holds_alternative<Lit>(parts[0]->node))
            return std::move(parts[0]);
        auto list = std::make_unique<Expr>();
        list->node = MakeList{std::move(parts), std::nullopt};
        return callE(
            "unicode", "characters_to_binary", 1, one(std::move(list)));
    }

    // ---- IR construction helpers -----------------------------------------
    auto litInt(long v) -> ExprPtr { return lit(LitKind::Int, std::to_string(v)); }
    auto one(ExprPtr a) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); return v;
    }
    auto two(ExprPtr a, ExprPtr b) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); v.push_back(std::move(b)); return v;
    }
    auto three(ExprPtr a, ExprPtr b, ExprPtr c) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v;
        v.push_back(std::move(a)); v.push_back(std::move(b)); v.push_back(std::move(c));
        return v;
    }
    auto callE_indirect(ExprPtr callee, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = CallIndirect{std::move(callee), std::move(args), false};
        return e;
    }
    // Atomize an already-lowered IR expr: bind to a fresh Let if it isn't a
    // Var/Lit, recording the binding.
    auto atomize_ir(ExprPtr e, std::vector<Binding>& binds) -> ExprPtr {
        if (std::holds_alternative<Var>(e->node) || std::holds_alternative<Lit>(e->node))
            return e;
        auto name = fresh();
        binds.push_back({name, std::move(e)});
        return var(name);
    }
    auto callE(std::string mod, std::string fn, int arity, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Call{std::move(mod), std::move(fn), arity, std::move(args), false};
        return e;
    }
    auto intrin(Op op, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Intrinsic{op, std::move(args)};
        return e;
    }

    // Run `body`, then kex_test:maybe_print_summary() (which prints the
    // describe/it pass/fail tally, or nothing if no tests ran), returning
    // body's value. Wraps every main.
    auto withTestSummary(ExprPtr body) -> ExprPtr {
        std::string r = fresh("R");
        return makeLet(r, std::move(body),
            makeLet(fresh("Sum"), callE("kex_test", "maybe_print_summary", 0, {}), var(r)));
    }

    // Payload arity per ADT variant tag (nullary variants lower to atoms and
    // never need display info).
    std::unordered_map<std::string, int> variantArity;
    std::unordered_map<std::string, std::string> variantOwner;

    // Prepend a kex_io:register_display/2 call carrying this module's record
    // layouts and variant arities — only the compiler knows which tuples are
    // records/variants, and the runtime needs that to render
    // `Name { field: value }` / `Tag(args)` instead of plain tuples.
    auto withDisplayInfo(ExprPtr body) -> ExprPtr {
        bool anyVariant = !variantArity.empty();
        if (records.empty() && !anyVariant) return std::move(body);
        auto atomLit = [&](const std::string& s) { return lit(LitKind::Atom, s); };
        auto mapFrom = [&](std::vector<ExprPtr> pairs) {
            auto lst = std::make_unique<Expr>();
            lst->node = MakeList{std::move(pairs), std::nullopt};
            return callE("maps", "from_list", 1, one(std::move(lst)));
        };
        std::vector<ExprPtr> recPairs;
        for (const auto& [name, info] : records) {
            std::vector<ExprPtr> fields;
            for (const auto& f : info.fields) fields.push_back(atomLit(f));
            auto fl = std::make_unique<Expr>();
            fl->node = MakeList{std::move(fields), std::nullopt};
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{two(atomLit(name), std::move(fl))};
            recPairs.push_back(std::move(t));
        }
        std::vector<ExprPtr> varPairs;
        for (const auto& [tag, ar] : variantArity) {
            auto metadata = std::make_unique<Expr>();
            metadata->node = MakeTuple{two(litInt(ar), atomLit(variantOwner[tag]))};
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{two(atomLit(tag), std::move(metadata))};
            varPairs.push_back(std::move(t));
        }
        return makeLet(fresh("Disp"),
            callE("kex_io", "register_display", 2,
                  two(mapFrom(std::move(recPairs)), mapFrom(std::move(varPairs)))),
            std::move(body));
    }

    // ---- Constructors -----------------------------------------------------
    auto makeLet(std::string name, ExprPtr value, ExprPtr body) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Let{std::move(name), std::move(value), std::move(body)};
        return e;
    }
    // A single-clause match binding `pattern` against `subject`, continuing
    // with `body` — used for `let (a, b) = expr` destructuring.
    // Single-clause destructuring (`let Just(x) = ...`, for-loop item patterns,
    // fold state patterns). Every caller treats a non-match as an error, which
    // is what the interpreter reports as "pattern mismatch" — so emit that
    // explicitly. Without a catch-all the emitted `case` has no fallback, and
    // Core Erlang's compiler crashes on the implicit failure with an internal
    // error in beam_core_to_ssa rather than producing a runnable module.
    auto makeMatch1(ExprPtr subject, PatternPtr pattern, ExprPtr body) -> ExprPtr {
        Match m;
        m.subjects.push_back(std::move(subject));
        MatchClause mc;
        mc.patterns.push_back(std::move(pattern));
        mc.body = std::move(body);
        m.clauses.push_back(std::move(mc));
        MatchClause fallback;
        auto wild = std::make_unique<Pattern>();
        wild->kind = PatKind::Wild;
        fallback.patterns.push_back(std::move(wild));
        fallback.body = runtimeError("pattern mismatch");
        m.clauses.push_back(std::move(fallback));
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        return e;
    }
    auto matchBool(ExprPtr cond, ExprPtr thenE, ExprPtr elseE) -> ExprPtr {
        Match m;
        m.subjects.push_back(std::move(cond));
        auto boolPat = [](bool b) {
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Lit; p->litKind = LitKind::Bool; p->litBool = b;
            return p;
        };
        MatchClause t; t.patterns.push_back(boolPat(true));  t.body = std::move(thenE);
        MatchClause f; f.patterns.push_back(boolPat(false)); f.body = std::move(elseE);
        m.clauses.push_back(std::move(t));
        m.clauses.push_back(std::move(f));
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        return e;
    }

    // ---- Patterns ---------------------------------------------------------
    auto wildPat() -> PatternPtr {
        auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
    }

    // ---- Guards -------------------------------------------------------------
    // Expand user-written `when` guards into nested matches. Guards lower as
    // ordinary expressions (BEAM guards can't hold arbitrary calls, so the
    // predicate moves out of guard position entirely). A guarded clause
    // `P when G -> B` becomes `P -> case G of true -> B; _ -> Cont() end`
    // followed by a wildcard clause `_ -> Cont()`, where Cont is a LetRec-bound
    // 0-arity function holding the expansion of the remaining clauses — so
    // clause bodies are never duplicated. A non-`true` guard result fails the
    // clause, matching native guard semantics. `receive` guards are NOT
    // expanded here (they must stay native; see lowerReceive).
    auto expandGuards(std::vector<ExprPtr> subjects, std::vector<MatchClause> clauses) -> ExprPtr {
        size_t gi = 0;
        while (gi < clauses.size() && !clauses[gi].guard) ++gi;
        if (gi == clauses.size()) {
            auto e = std::make_unique<Expr>();
            Match m; m.subjects = std::move(subjects); m.clauses = std::move(clauses);
            e->node = std::move(m);
            return e;
        }
        // Bind non-variable subjects so the continuation can re-match them.
        std::vector<std::string> subjNames;
        std::vector<std::pair<std::string, ExprPtr>> binds;
        for (auto& s : subjects) {
            if (auto* v = std::get_if<Var>(&s->node)) {
                subjNames.push_back(v->name);
            } else {
                std::string sv = fresh("Gsubj");
                binds.push_back({sv, std::move(s)});
                subjNames.push_back(sv);
            }
        }
        auto subjExprs = [&] {
            std::vector<ExprPtr> v;
            for (const auto& n : subjNames) v.push_back(var(n));
            return v;
        };
        // Continuation: expand the remaining clauses, or reproduce the
        // no-clause-matched failure when none remain.
        std::vector<MatchClause> rest(
            std::make_move_iterator(clauses.begin() + gi + 1),
            std::make_move_iterator(clauses.end()));
        ExprPtr contBody;
        if (rest.empty()) {
            auto tup = std::make_unique<Expr>();
            tup->node = MakeTuple{two(lit(LitKind::Atom, "case_clause"), var(subjNames[0]))};
            contBody = std::make_unique<Expr>();
            contBody->node = Call{"erlang", "error", 1, one(std::move(tup)), false};
        } else {
            contBody = expandGuards(subjExprs(), std::move(rest));
        }
        std::string contName = "__gcont" + std::to_string(counter++);
        auto callCont = [&] {
            auto c = std::make_unique<Expr>();
            c->node = Call{"", contName, 0, {}, false};
            return c;
        };
        Match m;
        m.subjects = subjExprs();
        for (size_t i = 0; i < gi; ++i) m.clauses.push_back(std::move(clauses[i]));
        MatchClause g;
        // An all-var/wildcard pattern already matches everything, so the
        // trailing fallback below would be dead code (erlc warns about it);
        // the guard's own `_ -> cont` arm covers the failure path.
        bool irrefutable = true;
        for (const auto& p : clauses[gi].patterns)
            if (p->kind != PatKind::Var && p->kind != PatKind::Wild)
                irrefutable = false;
        g.patterns = std::move(clauses[gi].patterns);
        // `true -> body; _ -> cont` — a non-boolean guard fails the clause.
        Match gm;
        gm.subjects.push_back(std::move(*clauses[gi].guard));
        MatchClause gt;
        auto tp = std::make_unique<Pattern>();
        tp->kind = PatKind::Lit; tp->litKind = LitKind::Bool; tp->litBool = true;
        gt.patterns.push_back(std::move(tp));
        gt.body = std::move(clauses[gi].body);
        MatchClause gf;
        gf.patterns.push_back(wildPat());
        gf.body = callCont();
        gm.clauses.push_back(std::move(gt));
        gm.clauses.push_back(std::move(gf));
        auto ge = std::make_unique<Expr>();
        ge->node = std::move(gm);
        g.body = std::move(ge);
        m.clauses.push_back(std::move(g));
        if (!irrefutable) {
            MatchClause fall;
            for (size_t i = 0; i < m.subjects.size(); ++i) fall.patterns.push_back(wildPat());
            fall.body = callCont();
            m.clauses.push_back(std::move(fall));
        }
        auto caseE = std::make_unique<Expr>();
        caseE->node = std::move(m);
        LetRec lr; lr.name = contName; lr.params = {};
        lr.funBody = std::move(contBody);
        lr.contBody = std::move(caseE);
        auto e = std::make_unique<Expr>();
        e->node = std::move(lr);
        for (auto it = binds.rbegin(); it != binds.rend(); ++it)
            e = makeLet(it->first, std::move(it->second), std::move(e));
        return e;
    }

    // Record-destructure `{ f, g: alias, h: { nested } }` → a flat list of
    // (name, accessor-expr) bindings read by field name from `baseVar`.
    // Recurses into nested record patterns.
    // Read field `fname` from `base`. Prefer a direct element(pos, base) from
    // the record layout — the accessor FUNCTION may be suppressed by a
    // same-named user method (e.g. records.kex's `city` field vs `city`
    // method), so calling `apply 'fname'/1` could hit the wrong function.
    auto fieldAccess(const std::string& fname, ExprPtr base) -> ExprPtr {
        auto it = fieldAccessors.find(fname);
        if (it != fieldAccessors.end() && !it->second.empty())
            return callE("erlang", "element", 2,
                two(litInt(it->second[0].second), std::move(base)));
        return callE("", fname, 1, one(std::move(base)));
    }
    // Field read when the record's type IS known — a named pattern `Foo { x }`.
    // Resolving by field name alone (fieldAccess above) takes the FIRST record
    // in the program declaring that name, which for common names like `value`
    // or `rest` is the prelude's 5-field ParseError. That yields silently wrong
    // tuple offsets for the user's own record. With a type name in hand, index
    // its declared layout directly.
    auto fieldAccessIn(const std::string& typeName, const std::string& fname,
                       ExprPtr base) -> ExprPtr {
        if (!typeName.empty()) {
            if (auto rec = records.find(typeName); rec != records.end()) {
                const auto& fs = rec->second.fields;
                for (size_t i = 0; i < fs.size(); i++)
                    if (fs[i] == fname)
                        return callE("erlang", "element", 2,
                                     two(litInt(static_cast<int>(i) + 2),
                                         std::move(base)));
            }
        }
        return fieldAccess(fname, std::move(base));
    }
    void destructureRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                                  std::vector<std::pair<std::string, ExprPtr>>& prefix) {
        for (const auto& field : rp.fields) {
            auto acc = fieldAccessIn(rp.typeName, field.name, var(baseVar));
            if (!field.pattern) {
                prefix.push_back({field.name, std::move(acc)});
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*field.pattern)->kind)) {
                prefix.push_back({vp->name, std::move(acc)});
            } else if (auto* nrp = std::get_if<ast::RecordPattern>(&(*field.pattern)->kind)) {
                std::string sub = fresh("rec");
                prefix.push_back({sub, std::move(acc)});
                destructureRecordPattern(sub, *nrp, prefix);
            } else {
                throw LowerError("IR lower: unsupported record-field sub-pattern");
            }
        }
    }
    // Function-head record patterns also need to preserve non-variable field
    // constraints (`{ x: 0.0 }`, tuple/list patterns). Register the variables
    // before lowering the clause body, then wrap that body in field reads and
    // Core Erlang matches so the constraint is enforced at call time.
    void collectRecordBindings(const ast::RecordPattern& rp, std::vector<std::string>& names) {
        for (const auto& field : rp.fields) {
            if (!field.pattern) {
                names.push_back(field.name);
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*field.pattern)->kind)) {
                names.push_back(vp->name);
            } else if (auto* nested = std::get_if<ast::RecordPattern>(&(*field.pattern)->kind)) {
                collectRecordBindings(*nested, names);
            }
        }
    }
    auto wrapRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                           ExprPtr body) -> ExprPtr {
        for (auto it = rp.fields.rbegin(); it != rp.fields.rend(); ++it) {
            auto value = fieldAccessIn(rp.typeName, it->name, var(baseVar));
            if (!it->pattern) {
                body = makeLet(it->name, std::move(value), std::move(body));
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*it->pattern)->kind)) {
                body = makeLet(vp->name, std::move(value), std::move(body));
            } else if (auto* nested = std::get_if<ast::RecordPattern>(&(*it->pattern)->kind)) {
                std::string sub = fresh("rec");
                body = wrapRecordPattern(sub, *nested, std::move(body));
                body = makeLet(sub, std::move(value), std::move(body));
            } else {
                body = makeMatch1(std::move(value), lowerPattern(*it->pattern), std::move(body));
            }
        }
        return body;
    }
    // A top-level destructuring `let` has to expose every name it binds as its
    // own 0-arity function, the same shape a simple `let name = value` gets.
    // Each one re-runs the right-hand side and keeps its own component, which
    // is already how top-level constants behave.
    auto topLevelPatternBinding(const ast::LetExpr& le, const std::string& name)
        -> ExprPtr {
        if (auto* rp = std::get_if<ast::RecordPattern>(&le.pattern->kind)) {
            std::string rv = fresh("rec");
            std::vector<std::pair<std::string, ExprPtr>> prefix;
            destructureRecordPattern(rv, *rp, prefix);
            for (auto& [nm, _] : prefix) subst[nm] = nm;
            ExprPtr body = var(name);
            for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                body = makeLet(it->first, std::move(it->second), std::move(body));
            return makeLet(rv, lower(le.value), std::move(body));
        }
        auto val = lower(le.value);
        auto pat = lowerPattern(le.pattern);
        return makeMatch1(std::move(val), std::move(pat), var(name));
    }
    auto lowerPattern(const ast::PatternPtr& p) -> PatternPtr {
        if (!p) { auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w; }
        return std::visit([&](const auto& pn) -> PatternPtr {
            using T = std::decay_t<decltype(pn)>;
            auto out = std::make_unique<Pattern>();
            if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
                out->kind = PatKind::Wild;
            } else if constexpr (std::is_same_v<T, ast::VarPattern>) {
                out->kind = PatKind::Var; out->name = pn.name;
                subst[pn.name] = pn.name; // pattern vars are binding sites
            } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
                out->kind = PatKind::Lit;
                switch (pn.literal.type) {
                    case TokenType::Integer: out->litKind = LitKind::Int; out->litText = pn.literal.value; break;
                    case TokenType::Float:   out->litKind = LitKind::Float; out->litText = pn.literal.value; break;
                    case TokenType::String:
                    case TokenType::RawString:
                        out->litKind = LitKind::String;
                        out->litText = pn.literal.value;
                        break;
                    case TokenType::Char:    out->litKind = LitKind::Char; out->litText = pn.literal.value; break;
                    case TokenType::True:    out->litKind = LitKind::Bool; out->litBool = true; break;
                    case TokenType::False:   out->litKind = LitKind::Bool; out->litBool = false; break;
                    case TokenType::Atom:    out->litKind = LitKind::Atom; out->litText = pn.literal.value; break;
                    case TokenType::None:    out->litKind = LitKind::None; out->litText = "none"; break;
                    default: throw LowerError("IR lower: unsupported literal pattern");
                }
            } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                out->kind = PatKind::Construct; out->tag = pn.name;
                for (const auto& a : pn.args) out->args.push_back(lowerPattern(a));
            } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                out->kind = PatKind::Tuple;
                for (const auto& a : pn.elements) out->args.push_back(lowerPattern(a));
            } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                out->kind = PatKind::List;
                for (const auto& a : pn.elements) out->args.push_back(lowerPattern(a));
                if (pn.rest) out->rest = lowerPattern(*pn.rest);
            } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                return pn.inner ? lowerPattern(pn.inner)
                                : [&]{ auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w; }();
            } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
                // A record is a tagged tuple {'Name', f1, f2, ...} in
                // declaration order, so a NAMED pattern is just a constructor
                // pattern: wildcard every field the pattern doesn't mention.
                // That asserts the record's type structurally, which is exactly
                // what `Foo { x }` means — no extra guard required.
                if (pn.typeName.empty())
                    throw LowerError(
                        "IR lower: anonymous record pattern `{ ... }` is not "
                        "supported in match/if-let; name the record "
                        "(`Foo { ... }`) so its field layout is known");
                auto rec = records.find(pn.typeName);
                if (rec == records.end())
                    throw LowerError("IR lower: unknown record `" + pn.typeName +
                                     "` in pattern");
                out->kind = PatKind::Construct;
                out->tag = pn.typeName;
                for (const auto& fieldName : rec->second.fields) {
                    const ast::FieldPattern* bound = nullptr;
                    for (const auto& f : pn.fields)
                        if (!f.isStringKey && f.name == fieldName) { bound = &f; break; }
                    if (!bound) {
                        auto w = std::make_unique<Pattern>();
                        w->kind = PatKind::Wild;
                        out->args.push_back(std::move(w));
                    } else if (bound->pattern && *bound->pattern) {
                        out->args.push_back(lowerPattern(*bound->pattern));
                    } else {
                        // Shorthand `{ x }` binds the field to its own name.
                        auto v = std::make_unique<Pattern>();
                        v->kind = PatKind::Var;
                        v->name = bound->name;
                        subst[bound->name] = bound->name;
                        out->args.push_back(std::move(v));
                    }
                }
            } else {
                throw LowerError("IR lower: unsupported pattern kind");
            }
            return out;
        }, p->kind);
    }

    // ---- Control flow -----------------------------------------------------
    // Lower a branch body in its own scope (branch-local bindings must not
    // leak into sibling branches or the code after the `if`/`match`).
    auto lowerBodyScoped(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
        auto snap = subst;
        auto r = lowerBody(body);
        subst = snap;
        return r;
    }

    auto lowerIf(const ast::IfExpr& n) -> ExprPtr {
        // `if let Pat = scrutinee ... [else ...]`: match Pat, else fall through.
        if (n.letPattern) {
            auto subj = lower(n.condition); // condition holds the scrutinee
            auto snap = subst;
            auto pat = lowerPattern(n.letPattern);
            auto thenP = lowerBody(n.thenBody);
            subst = snap;
            auto elseP = n.elseBody ? lowerBodyScoped(*n.elseBody) : lit(LitKind::Atom, "ok");
            Match m; m.subjects.push_back(std::move(subj));
            MatchClause hit; hit.patterns.push_back(std::move(pat)); hit.body = std::move(thenP);
            MatchClause miss; auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild;
            miss.patterns.push_back(std::move(w)); miss.body = std::move(elseP);
            m.clauses.push_back(std::move(hit)); m.clauses.push_back(std::move(miss));
            auto e = std::make_unique<Expr>(); e->node = std::move(m); return e;
        }
        ExprPtr elsePart = n.elseBody ? lowerBodyScoped(*n.elseBody)
                                      : lit(LitKind::Atom, "ok");
        for (int i = static_cast<int>(n.elifs.size()) - 1; i >= 0; --i) {
            auto cond = lower(n.elifs[i].first);
            auto thenP = lowerBodyScoped(n.elifs[i].second);
            elsePart = matchBool(std::move(cond), std::move(thenP), std::move(elsePart));
        }
        auto cond = lower(n.condition);
        auto thenP = lowerBodyScoped(n.thenBody);
        return matchBool(std::move(cond), std::move(thenP), std::move(elsePart));
    }

    auto lowerMatch(const ast::MatchExpr& n) -> ExprPtr {
        // Range-pattern dispatch: ranges materialize as ascending lists, so a
        // `(x..y)` / `(-10..-1)` clause can't match structurally. Bind the
        // subject once, then dispatch on its <first, last> bounds as a
        // 2-subject match — each RangePattern becomes <startPat, endPat>.
        bool rangeMode = std::any_of(n.clauses.begin(), n.clauses.end(),
            [](const ast::MatchClause& c) {
                return !c.patterns.empty() &&
                       std::holds_alternative<ast::RangePattern>(c.patterns[0]->kind);
            });
        if (rangeMode) return lowerRangeMatch(n);

        // `match subj do |x| ... end` binds the subject to `x`, in scope for
        // every clause's guard/body → let-bind it and match the bound var.
        ExprPtr subjIr = lower(n.subject);
        ExprPtr letWrap;
        std::string subjVar;
        std::optional<std::string> subjPrev; // outer mapping to restore on exit
        if (n.subjectBinding) {
            subjVar = *n.subjectBinding;
            if (auto it = subst.find(subjVar); it != subst.end()) subjPrev = it->second;
            std::string ssa = subjPrev ? fresh(subjVar) : subjVar;
            subst[subjVar] = ssa;
            letWrap = std::move(subjIr);
            subjIr = var(ssa);
        }
        std::vector<ExprPtr> subjects;
        subjects.push_back(std::move(subjIr));
        std::vector<MatchClause> cls;
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            MatchClause mc;
            for (const auto& p : cl.patterns) mc.patterns.push_back(lowerPattern(p));
            // `when` guards lower as ordinary expressions; expandGuards moves
            // the predicate out of BEAM guard position into a nested match.
            if (cl.guard) mc.guard = lower(*cl.guard);
            mc.body = lower(cl.body);
            subst = snap;
            cls.push_back(std::move(mc));
        }
        auto e = expandGuards(std::move(subjects), std::move(cls));
        if (letWrap) {
            auto r = makeLet(currentName(subjVar), std::move(letWrap), std::move(e));
            if (subjPrev) subst[subjVar] = *subjPrev; else subst.erase(subjVar);
            return r;
        }
        return e;
    }

    auto lowerReceive(const ast::ReceiveExpr& n) -> ExprPtr {
        Receive r;
        r.senderVar = n.senderBinding ? *n.senderBinding : fresh("Sndr");
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            if (n.senderBinding) subst[*n.senderBinding] = *n.senderBinding;
            ReceiveClause rc;
            rc.pattern = cl.patterns.empty() ? wildPat() : lowerPattern(cl.patterns[0]);
            rc.body = lower(cl.body);
            subst = snap;
            r.clauses.push_back(std::move(rc));
        }
        if (n.timeout && n.afterBody) {
            r.timeout = lower(*n.timeout);
            r.afterBody = lower(*n.afterBody);
        }
        auto e = std::make_unique<Expr>(); e->node = std::move(r);
        return e;
    }

    // A match with range-pattern clauses. Bind the subject, then match on its
    // <hd, last> bounds; each `(a..b)` clause → the 2-pattern `<a, b>`.
    auto lowerRangeMatch(const ast::MatchExpr& n) -> ExprPtr {
        std::string sv = fresh("RSubj");
        auto subjVal = lower(n.subject);
        std::vector<ExprPtr> subjects;
        subjects.push_back(callE("erlang", "hd", 1, one(var(sv))));
        subjects.push_back(callE("lists", "last", 1, one(var(sv))));
        std::vector<MatchClause> cls;
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            MatchClause mc;
            const auto& p0 = cl.patterns[0];
            if (auto* rp = std::get_if<ast::RangePattern>(&p0->kind)) {
                mc.patterns.push_back(rp->start ? lowerPattern(rp->start) : wildPat());
                mc.patterns.push_back(rp->end ? lowerPattern(rp->end) : wildPat());
            } else {
                // A non-range clause (e.g. a bare `_`) spans both bounds.
                mc.patterns.push_back(wildPat());
                mc.patterns.push_back(wildPat());
            }
            if (cl.guard) mc.guard = lower(*cl.guard);
            mc.body = lower(cl.body);
            subst = snap;
            cls.push_back(std::move(mc));
        }
        auto e = expandGuards(std::move(subjects), std::move(cls));
        return makeLet(sv, std::move(subjVal), std::move(e));
    }

    // ---- Loops ------------------------------------------------------------
    // Collect the names a loop body reassigns — plain `x = ...` AND mutating
    // `x.push!(..)` calls — recursing through if/match/block/nested-loops but
    // not lambdas. These become the loop's threaded state.
    void collectMutated(const ast::ExprPtr& e, std::unordered_set<std::string>& out) {
        if (!e) return;
        std::visit([&](const auto& nn) {
            using T = std::decay_t<decltype(nn)>;
            if constexpr (std::is_same_v<T, ast::AssignExpr>) out.insert(nn.name);
            else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (nn.mutating && nn.receiver)
                    if (auto* id = std::get_if<ast::Identifier>(&nn.receiver->kind)) out.insert(id->name);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                for (auto& s : nn.thenBody) collectMutated(s, out);
                if (nn.elseBody) for (auto& s : *nn.elseBody) collectMutated(s, out);
                for (auto& [c, b] : nn.elifs) for (auto& s : b) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                for (auto& cl : nn.clauses) collectMutated(cl.body, out);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
                for (auto& cl : nn.rescue.clauses) collectMutated(cl.body, out);
                for (auto& s : nn.rescue.catchAllBody) collectMutated(s, out);
            }
        }, e->kind);
    }

    // The loop's current threaded-state value: bare var for one, tuple for
    // several, 'ok' for none.
    auto stateExpr(const std::vector<std::string>& mutVars) -> ExprPtr {
        if (mutVars.empty()) return lit(LitKind::Atom, "ok");
        if (mutVars.size() == 1) return var(currentName(mutVars[0]));
        std::vector<ExprPtr> els;
        for (const auto& v : mutVars) els.push_back(var(currentName(v)));
        auto e = std::make_unique<Expr>(); e->node = MakeTuple{std::move(els)}; return e;
    }
    auto tailCall(const std::string& loopFn, const std::vector<std::string>& mutVars) -> ExprPtr {
        std::vector<ExprPtr> args;
        for (const auto& v : mutVars) args.push_back(var(currentName(v)));
        auto e = std::make_unique<Expr>();
        e->node = Call{"", loopFn, (int)mutVars.size(), std::move(args), false};
        return e;
    }

    // If `e` is a break/next/return, its loop-control IR; else nullptr.
    auto loopControl(const ast::ExprPtr& e, const std::string& loopFn,
                     const std::vector<std::string>& mutVars) -> ExprPtr {
        if (std::holds_alternative<ast::BreakExpr>(e->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(e->kind)) return tailCall(loopFn, mutVars);
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        return nullptr;
    }

    // Lower a loop body's statements in loop context. `onEnd()` is spliced in
    // once the statements are exhausted — for the loop's own top-level body it
    // tail-calls the loop (next iteration); for an if/match branch it is "the
    // rest of the enclosing loop body". break yields the state, next
    // tail-calls, assignments/mutations rebind and thread forward, and
    // if/match/nested-loop recurse with the appropriate continuation. This one
    // callback-driven pass replaces what the string emitter did with three
    // separate, partly-overlapping loop-body walkers.
    auto lowerLoopBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i,
                           const std::string& loopFn,
                           const std::vector<std::string>& mutVars,
                           const std::function<ExprPtr()>& onEnd) -> ExprPtr {
        if (i >= body.size()) return onEnd();
        const auto& e = body[i];
        auto cont = [&]() -> ExprPtr { return lowerLoopBodyFrom(body, i + 1, loopFn, mutVars, onEnd); };
        if (std::holds_alternative<ast::BreakExpr>(e->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(e->kind)) return tailCall(loopFn, mutVars);
        if (auto* ti = std::get_if<ast::TrailingIf>(&e->kind))
            if (auto ctrl = loopControl(ti->expr, loopFn, mutVars)) {
                // The condition must be lowered against the subst in effect
                // HERE: cont() lowers the rest of the loop body, which
                // rebinds mutated vars to fresh SSA names. Passing both as
                // arguments to matchBool() leaves them unsequenced, and a
                // compiler that evaluates right-to-left (gcc on x86_64) then
                // renders the condition with names bound later in the body —
                // an unbound variable in the emitted Core Erlang.
                auto c = lower(ti->condition);
                auto rest = cont();
                return matchBool(std::move(c), std::move(ctrl), std::move(rest));
            }
        if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            auto val = lower(ae->value);
            std::string nv = fresh(ae->name); subst[ae->name] = nv;
            return makeLet(nv, std::move(val), cont());
        }
        if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            auto val = lower(ve->value);
            std::string nv = fresh(ve->name); subst[ve->name] = nv;
            immutableBindings.erase(ve->name);
            return makeLet(nv, std::move(val), cont());
        }
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string nv = fresh(vp->name); subst[vp->name] = nv;
                immutableBindings.insert(vp->name);
                return makeLet(nv, std::move(val), cont());
            }
            auto val = lower(le->value); auto pat = lowerPattern(le->pattern);
            // Collect destructured names as immutable.
            if (le->pattern)
                if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                    immutableBindings.insert(vp->name);
            return makeMatch1(std::move(val), std::move(pat), cont());
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver)
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                if (immutableBindings.count(id->name)) {
                    std::string loc;
                    if (currentLoc) loc = std::string(currentLoc->file) + ":"
                        + std::to_string(currentLoc->line) + ":"
                        + std::to_string(currentLoc->column) + ": ";
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"erlang", "error", 1,
                        one(lit(LitKind::String, loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)), false};
                    return std::move(ex);
                }
                auto val = lowerMutatingAsValue(*mc);
                std::string nv = fresh(id->name); subst[id->name] = nv;
                return makeLet(nv, std::move(val), cont());
            }
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
            // `return X if cond` (ReturnExpr wrapping a TrailingIf): return
            // only when cond holds, otherwise fall through to the rest.
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                // Sequenced for the same reason as the TrailingIf case above.
                auto c = lower(ti->condition);
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                auto rest = cont();
                return matchBool(std::move(c), std::move(retX), std::move(rest));
            }
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        if (auto* ie = std::get_if<ast::IfExpr>(&e->kind)) {
            std::function<ExprPtr()> branchEnd = [&]() -> ExprPtr { return cont(); };
            auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                auto snap = subst;
                auto r = lowerLoopBodyFrom(bb, 0, loopFn, mutVars, branchEnd);
                subst = snap; return r;
            };
            auto c = lower(ie->condition);
            auto thenP = branch(ie->thenBody);
            ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody) : cont();
            return matchBool(std::move(c), std::move(thenP), std::move(elseP));
        }
        if (auto* me = std::get_if<ast::MatchExpr>(&e->kind)) {
            if (me->subjectBinding)
                throw LowerError("IR lower: match |binder| in loop not yet ported");
            std::function<ExprPtr()> armEnd = [&]() -> ExprPtr { return cont(); };
            std::vector<ExprPtr> subjects;
            subjects.push_back(lower(me->subject));
            std::vector<MatchClause> cls;
            for (const auto& cl : me->clauses) {
                auto snap = subst;
                MatchClause mcx;
                for (const auto& p : cl.patterns) mcx.patterns.push_back(lowerPattern(p));
                if (cl.guard) mcx.guard = lower(*cl.guard);
                mcx.body = lowerLoopArmU(cl.body, loopFn, mutVars, armEnd);
                subst = snap;
                cls.push_back(std::move(mcx));
            }
            return expandGuards(std::move(subjects), std::move(cls));
        }
        if (auto* le2 = std::get_if<ast::LoopExpr>(&e->kind))
            return lowerLoopCore(le2->body, nullptr, false, [&]{ return cont(); });
        if (auto* we2 = std::get_if<ast::WhileExpr>(&e->kind))
            return lowerLoopCore(we2->body, &we2->condition, false, [&]{ return cont(); });
        if (auto* te = std::get_if<ast::TryingExpr>(&e->kind)) {
            TryCatch tc;
            auto snapBeforeTry = subst;
            tc.body = lowerLoopBodyFrom(te->body, 0, loopFn, mutVars, onEnd);
            // Restore subst so rescue clauses reference pre-try variable names
            subst = snapBeforeTry;
            auto& rescue = te->rescue;
            if (rescue.isInlineReturn) {
                MatchClause c;
                c.patterns.push_back(wildPat());
                auto retExpr = std::make_unique<Expr>();
                retExpr->node = Return{lower(rescue.inlineReturnExpr)};
                c.body = std::move(retExpr);
                tc.clauses.push_back(std::move(c));
            } else if (rescue.isCatchAll) {
                MatchClause c;
                if (rescue.catchAllParam.empty()) {
                    c.patterns.push_back(wildPat());
                } else {
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Var;
                    p->name = rescue.catchAllParam;
                    subst[rescue.catchAllParam] = rescue.catchAllParam;
                    c.patterns.push_back(std::move(p));
                }
                c.body = lowerLoopBodyFrom(rescue.catchAllBody, 0, loopFn, mutVars, onEnd);
                tc.clauses.push_back(std::move(c));
            } else {
                for (const auto& clause : rescue.clauses) {
                    MatchClause mc;
                    if (!clause.patterns.empty())
                        mc.patterns.push_back(lowerPattern(clause.patterns[0]));
                    else
                        mc.patterns.push_back(wildPat());
                    mc.body = clause.body
                        ? lowerLoopArmU(clause.body, loopFn, mutVars, [&]{ return cont(); })
                        : cont();
                    tc.clauses.push_back(std::move(mc));
                }
            }
            auto ex = std::make_unique<Expr>();
            ex->node = std::move(tc);
            return ex;
        }
        auto val = lower(e);
        return makeLet(fresh("S"), std::move(val), cont());
    }

    // A single match-arm body in loop context (a `do` block is a statement
    // list; a bare expr is one statement), continuing with `armEnd`.
    auto lowerLoopArmU(const ast::ExprPtr& arm, const std::string& loopFn,
                       const std::vector<std::string>& mutVars,
                       const std::function<ExprPtr()>& armEnd) -> ExprPtr {
        if (auto* be = std::get_if<ast::BlockExpr>(&arm->kind))
            return lowerLoopBodyFrom(be->body, 0, loopFn, mutVars, armEnd);
        if (std::holds_alternative<ast::BreakExpr>(arm->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(arm->kind)) return tailCall(loopFn, mutVars);
        if (auto* ti = std::get_if<ast::TrailingIf>(&arm->kind))
            if (auto ctrl = loopControl(ti->expr, loopFn, mutVars)) {
                // armEnd() rebinds mutated vars — lower the condition first.
                auto c = lower(ti->condition);
                auto rest = armEnd();
                return matchBool(std::move(c), std::move(ctrl), std::move(rest));
            }
        if (auto* re = std::get_if<ast::ReturnExpr>(&arm->kind)) {
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                auto c = lower(ti->condition);
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                auto rest = armEnd();
                return matchBool(std::move(c), std::move(retX), std::move(rest));
            }
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        if (auto* ae = std::get_if<ast::AssignExpr>(&arm->kind)) {
            auto val = lower(ae->value);
            std::string nv = fresh(ae->name); subst[ae->name] = nv;
            return makeLet(nv, std::move(val), armEnd());
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&arm->kind); mc && mc->mutating && mc->receiver)
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                auto val = lowerMutatingAsValue(*mc);
                std::string nv = fresh(id->name); subst[id->name] = nv;
                return makeLet(nv, std::move(val), armEnd());
            }
        auto val = lower(arm);
        return makeLet(fresh("S"), std::move(val), armEnd());
    }

    // Lower a mutating `x.method!(args)` as the VALUE it rebinds x to (the
    // non-mutating method result).
    auto lowerMutatingAsValue(const ast::MethodCall& mc) -> ExprPtr {
        m_lowerMutatingAsValue = true;
        auto r = lowerMethodCall(mc);
        m_lowerMutatingAsValue = false;
        return r;
    }

    // Lower a `loop`/`while` to a LetRec threading its mutable state.
    // `restIsLast`: the loop is the last statement (its result is the value).
    // `mkRest`: produces the continuation after the loop (with the extracted
    // state already in subst) — for a top-level loop this is the rest of the
    // enclosing body; for a NESTED loop it's the rest of the OUTER loop body
    // (so it tail-calls the outer loop).
    auto lowerLoopCore(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       bool restIsLast, const std::function<ExprPtr()>& mkRest) -> ExprPtr {
        std::unordered_set<std::string> mset;
        for (const auto& s : loopBody) collectMutated(s, mset);
        std::vector<std::string> mutVars;
        for (const auto& v : mset) if (subst.count(v)) mutVars.push_back(v);
        std::sort(mutVars.begin(), mutVars.end());

        std::string loopFn = "__loop" + std::to_string(counter++);
        std::vector<ExprPtr> initArgs;
        for (const auto& v : mutVars) initArgs.push_back(var(currentName(v)));

        auto snap = subst;
        for (const auto& v : mutVars) subst[v] = v;
        // Condition and false-exit state use the ENTRY bindings.
        ExprPtr condExpr = cond ? lower(*cond) : nullptr;
        ExprPtr falseState = cond ? stateExpr(mutVars) : nullptr;
        ExprPtr inner = lowerLoopBodyFrom(loopBody, 0, loopFn, mutVars,
            [&]() -> ExprPtr { return tailCall(loopFn, mutVars); });
        if (cond)
            inner = matchBool(std::move(condExpr), std::move(inner), std::move(falseState));
        subst = snap;

        LetRec lr; lr.name = loopFn; lr.params = mutVars; lr.funBody = std::move(inner);
        auto callLoop = std::make_unique<Expr>();
        callLoop->node = Call{"", loopFn, (int)mutVars.size(), std::move(initArgs), false};

        if (mutVars.empty()) {
            lr.contBody = restIsLast ? std::move(callLoop)
                : makeLet(fresh("S"), std::move(callLoop), mkRest());
        } else {
            std::string resVar = fresh("LR");
            // Capture extracted names BEFORE mkRest() (which may itself lower a
            // loop that reassigns subst[v]).
            std::vector<std::string> boundNames;
            for (size_t k = 0; k < mutVars.size(); k++) {
                std::string nv = fresh(mutVars[k]); subst[mutVars[k]] = nv; boundNames.push_back(nv);
            }
            auto rest = restIsLast ? stateExpr(mutVars) : mkRest();
            ExprPtr chained = std::move(rest);
            if (mutVars.size() == 1) {
                chained = makeLet(boundNames[0], var(resVar), std::move(chained));
            } else {
                // Only unpack the loop-carried slots the continuation actually
                // reads: an `element/2` bound to a dead name makes erlc warn
                // that the call's result is ignored (loop counters hit this
                // constantly, since they're live only inside the loop).
                for (size_t k = mutVars.size(); k-- > 0; ) {
                    if (!mentionsVar(*chained, boundNames[k])) continue;
                    chained = makeLet(boundNames[k],
                        callE("erlang", "element", 2, two(litInt((long)k + 1), var(resVar))),
                        std::move(chained));
                }
            }
            lr.contBody = makeLet(resVar, std::move(callLoop), std::move(chained));
        }
        auto e = std::make_unique<Expr>(); e->node = std::move(lr);
        return e;
    }
    // Top-level loop statement: continuation is the rest of the enclosing body.
    auto lowerLoopStmt(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       const std::vector<ast::ExprPtr>& outer, size_t outerStart) -> ExprPtr {
        bool restIsLast = (outerStart + 1 >= outer.size());
        return lowerLoopCore(loopBody, cond, restIsLast,
            [&]{ return lowerBodyFrom(outer, outerStart + 1); });
    }

    // ---- Body lowering ----------------------------------------------------
    // A statement sequence. `let`/`var`/reassignments introduce SSA-renamed
    // bindings (updating `subst`); every other statement's value is bound to
    // a throwaway name; the last statement is the sequence's value.
    auto lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
        if (body.empty()) return lit(LitKind::Atom, "ok");
        return lowerBodyFrom(body, 0);
    }
    auto lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr {
        const auto& e = body[i];
        bool isLast = (i + 1 == body.size());
        auto prevLoc = currentLoc;
        currentLoc = &e->location;

        // let PATTERN = value
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
                subst[vp->name] = ssa;
                immutableBindings.insert(vp->name);
                auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
                return makeLet(ssa, std::move(val), std::move(rest));
            }
            // Record destructure `let { f, g: { h } } = value`: fields are read
            // by name, not structurally — bind the value then prepend accessors.
            if (auto* rp = std::get_if<ast::RecordPattern>(&le->pattern->kind)) {
                std::string rv = fresh("rec");
                std::vector<std::pair<std::string, ExprPtr>> prefix;
                destructureRecordPattern(rv, *rp, prefix);
                for (auto& [nm, _] : prefix) subst[nm] = nm;
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                    rest = makeLet(it->first, std::move(it->second), std::move(rest));
                return makeLet(rv, lower(le->value), std::move(rest));
            }
            auto val = lower(le->value);
            auto pat = lowerPattern(le->pattern);
            auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
            return makeMatch1(std::move(val), std::move(pat), std::move(rest));
        }
        // var name = value  (a fresh mutable binding)
        if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            auto val = lower(ve->value);
            std::string ssa = subst.count(ve->name) ? fresh(ve->name) : ve->name;
            subst[ve->name] = ssa;
            immutableBindings.erase(ve->name);
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
        // name = value  (reassignment → fresh SSA name, value sees the OLD one)
        if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            auto val = lower(ae->value);
            std::string ssa = fresh(ae->name);
            subst[ae->name] = ssa;
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
        // x.push!(v) / name.upperCase!  → rebind x to the (non-mutating) result.
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver) {
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                if (immutableBindings.count(id->name)) {
                    auto err = std::make_unique<Expr>();
                    std::string loc;
                    if (currentLoc) loc = std::string(currentLoc->file) + ":"
                        + std::to_string(currentLoc->line) + ":"
                        + std::to_string(currentLoc->column) + ": ";
                    err->node = Call{"erlang", "error", 1,
                        one(lit(LitKind::String, loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)), false};
                    return std::move(err);
                }
                auto val = lowerMutatingAsValue(*mc);
                std::string ssa = fresh(id->name);
                subst[id->name] = ssa;
                auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
                return makeLet(ssa, std::move(val), std::move(rest));
            }
        }
        // A closure captures the walker's mutable bindings by reference, but
        // BEAM closures only capture values. For finite iteration receiver
        // functions (`each` and `times`) whose callback reassigns an outer
        // variable, lower the iteration to lists:foldl and use the accumulator
        // as explicit mutable state. Once the callback completes, rebind that
        // state in the enclosing SSA scope.
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind);
            mc && (mc->method == "each" || mc->method == "times") &&
            mc->receiver && mc->args.empty()
            && mc->namedArgs.empty() && mc->block) {
            auto* lam = std::get_if<ast::Lambda>(&(*mc->block)->kind);
            std::unordered_set<std::string> muts;
            if (lam) for (const auto& stmt : lam->body) collectMutated(stmt, muts);
            std::vector<std::string> mutVars;
            for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
            std::sort(mutVars.begin(), mutVars.end());
            bool supportedArity = lam &&
                ((mc->method == "times" && lam->params.size() == 1) ||
                 (mc->method == "each" &&
                  (lam->params.size() == 1 || lam->params.size() == 2)));
            if (supportedArity && !mutVars.empty()) {
                std::vector<Binding> binds;
                auto receiver = atomize_ir(lower(mc->receiver), binds);
                auto snap = subst;
                std::string item = lam->params.size() == 1 ? lam->params[0].name : fresh("EachItem");
                std::string state = fresh("EachState");
                Lambda fold;
                fold.params = {item, state};
                if (lam->params.size() == 1) {
                    subst[lam->params[0].name] = item;
                } else {
                    subst[lam->params[0].name] = lam->params[0].name;
                    subst[lam->params[1].name] = lam->params[1].name;
                }
                auto bindItem = [&](ExprPtr callback) -> ExprPtr {
                    if (lam->params.size() == 1) return callback;
                    auto pair = std::make_unique<Pattern>();
                    pair->kind = PatKind::Tuple;
                    for (const auto& p : lam->params) {
                        auto name = std::make_unique<Pattern>();
                        name->kind = PatKind::Var; name->name = p.name;
                        pair->args.push_back(std::move(name));
                    }
                    return makeMatch1(var(item), std::move(pair), std::move(callback));
                };

                if (mutVars.size() == 1) {
                    subst[mutVars[0]] = state;
                    fold.body = bindItem(lowerLoopBodyFrom(lam->body, 0, "", mutVars,
                        [&] { return stateExpr(mutVars); }));
                } else {
                    auto statePat = std::make_unique<Pattern>();
                    statePat->kind = PatKind::Tuple;
                    for (const auto& v : mutVars) {
                        std::string sv = fresh(v);
                        subst[v] = sv;
                        auto p = std::make_unique<Pattern>();
                        p->kind = PatKind::Var; p->name = sv;
                        statePat->args.push_back(std::move(p));
                    }
                    auto body = lowerLoopBodyFrom(lam->body, 0, "", mutVars,
                        [&] { return stateExpr(mutVars); });
                    fold.body = makeMatch1(var(state), std::move(statePat), bindItem(std::move(body)));
                }
                subst = snap;

                auto foldExpr = std::make_unique<Expr>();
                foldExpr->node = std::move(fold);
                auto foldFn = atomize_ir(std::move(foldExpr), binds);
                auto initial = atomize_ir(stateExpr(mutVars), binds);
                ExprPtr items;
                if (mc->method == "times") {
                    auto last = intrin(Op::Sub,
                        two(std::move(receiver), litInt(1)));
                    items = callE("lists", "seq", 2,
                                  two(litInt(0), std::move(last)));
                } else {
                    items = callE("kex_intrinsic_fun", "items", 1,
                                  one(std::move(receiver)));
                }
                auto result = wrapLets(binds, callE("lists", "foldl", 3,
                    three(std::move(foldFn), std::move(initial), std::move(items))));

                if (mutVars.size() == 1) {
                    std::string nv = fresh(mutVars[0]);
                    subst[mutVars[0]] = nv;
                    auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                    return makeLet(nv, std::move(result), std::move(rest));
                }
                auto resultPat = std::make_unique<Pattern>();
                resultPat->kind = PatKind::Tuple;
                for (const auto& v : mutVars) {
                    std::string nv = fresh(v);
                    subst[v] = nv;
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Var; p->name = nv;
                    resultPat->args.push_back(std::move(p));
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                return makeMatch1(std::move(result), std::move(resultPat), std::move(rest));
            }
        }
        // Statement-position if/match whose branches REASSIGN outer vars —
        // thread the mutated state through, exactly like loops do: each
        // branch yields the (possibly reassigned) values, and the code after
        // sees fresh SSA names (`var sql = …; if c sql = sql + x end; …`).
        {
            auto* ie = std::get_if<ast::IfExpr>(&e->kind);
            auto* me = std::get_if<ast::MatchExpr>(&e->kind);
            if (ie || (me && !me->subjectBinding)) {
                std::unordered_set<std::string> muts;
                collectMutated(e, muts);
                std::vector<std::string> mutVars;
                for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
                for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
                std::sort(mutVars.begin(), mutVars.end());
                if (!mutVars.empty()) {
                    std::function<ExprPtr()> yieldState =
                        [&]() -> ExprPtr { return stateExpr(mutVars); };
                    ExprPtr caseE;
                    if (ie) {
                        auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                            auto snap = subst;
                            auto r = lowerLoopBodyFrom(bb, 0, "", mutVars, yieldState);
                            subst = snap; return r;
                        };
                        ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody)
                                                     : stateExpr(mutVars);
                        for (auto it2 = ie->elifs.rbegin(); it2 != ie->elifs.rend(); ++it2) {
                            auto ec = lower(it2->first);
                            auto eb = branch(it2->second);
                            elseP = matchBool(std::move(ec), std::move(eb),
                                              std::move(elseP));
                        }
                        auto cc = lower(ie->condition);
                        auto cb = branch(ie->thenBody);
                        caseE = matchBool(std::move(cc), std::move(cb),
                                          std::move(elseP));
                    } else {
                        std::vector<ExprPtr> subjects;
                        subjects.push_back(lower(me->subject));
                        std::vector<MatchClause> cls;
                        for (const auto& cl : me->clauses) {
                            auto snap = subst;
                            MatchClause mc;
                            for (const auto& p : cl.patterns)
                                mc.patterns.push_back(lowerPattern(p));
                            if (cl.guard) mc.guard = lower(*cl.guard);
                            mc.body = lowerLoopArmU(cl.body, "", mutVars, yieldState);
                            subst = snap;
                            cls.push_back(std::move(mc));
                        }
                        caseE = expandGuards(std::move(subjects), std::move(cls));
                    }
                    // Rebind the mutated names to the yielded values.
                    if (mutVars.size() == 1) {
                        std::string nv = fresh(mutVars[0]);
                        subst[mutVars[0]] = nv;
                        auto rest = isLast ? var(nv) : lowerBodyFrom(body, i + 1);
                        return makeLet(nv, std::move(caseE), std::move(rest));
                    }
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Tuple;
                    for (const auto& v : mutVars) {
                        std::string nv = fresh(v);
                        subst[v] = nv;
                        auto vp = std::make_unique<Pattern>();
                        vp->kind = PatKind::Var; vp->name = nv;
                        pat->args.push_back(std::move(vp));
                    }
                    auto rest = isLast ? lit(LitKind::Atom, "ok")
                                       : lowerBodyFrom(body, i + 1);
                    return makeMatch1(std::move(caseE), std::move(pat), std::move(rest));
                }
            }
        }
        // loop / while → tail-recursive local function threading mutable state.
        if (auto* le = std::get_if<ast::LoopExpr>(&e->kind))
            return lowerLoopStmt(le->body, nullptr, body, i);
        if (auto* we = std::get_if<ast::WhileExpr>(&e->kind))
            return lowerLoopStmt(we->body, &we->condition, body, i);

        // `return X if cond` as a non-last statement: return only when cond
        // holds, otherwise fall through to the rest (a plain ReturnExpr lowers
        // its value directly, so `return (cond ? X : ok)` would return
        // unconditionally — wrong). Mirrors the loop-body handling.
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind); re && !isLast) {
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                // lowerBodyFrom() rebinds mutated vars — sequence it last.
                auto c = lower(ti->condition);
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                auto rest = lowerBodyFrom(body, i + 1);
                return matchBool(std::move(c), std::move(retX), std::move(rest));
            }
        }

        if (isLast) return lower(e);
        auto val = lower(e);
        auto rest = lowerBodyFrom(body, i + 1);
        return makeLet(fresh("S"), std::move(val), std::move(rest));
    }

    // ---- Rescue -----------------------------------------------------------
    auto lowerRescueClauses(const ast::RescueBlock& rescue) -> std::vector<MatchClause> {
        std::vector<MatchClause> out;
        if (rescue.isInlineReturn) {
            MatchClause c;
            c.patterns.push_back(wildPat());
            auto retExpr = std::make_unique<Expr>();
            retExpr->node = Return{lower(rescue.inlineReturnExpr)};
            c.body = std::move(retExpr);
            out.push_back(std::move(c));
        } else if (rescue.isCatchAll) {
            MatchClause c;
            if (rescue.catchAllParam.empty()) {
                c.patterns.push_back(wildPat());
            } else {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = rescue.catchAllParam;
                subst[rescue.catchAllParam] = rescue.catchAllParam;
                c.patterns.push_back(std::move(p));
            }
            c.body = lowerBody(rescue.catchAllBody);
            out.push_back(std::move(c));
        } else {
            for (const auto& clause : rescue.clauses) {
                MatchClause mc;
                if (!clause.patterns.empty()) {
                    mc.patterns.push_back(lowerPattern(clause.patterns[0]));
                } else {
                    mc.patterns.push_back(wildPat());
                }
                mc.body = clause.body ? lower(clause.body) : lit(LitKind::Atom, "ok");
                out.push_back(std::move(mc));
            }
        }
        return out;
    }

    auto wrapWithTryCatch(ExprPtr body, const ast::RescueBlock& rescue) -> ExprPtr {
        TryCatch tc;
        tc.body = std::move(body);
        tc.clauses = lowerRescueClauses(rescue);
        auto ex = std::make_unique<Expr>();
        ex->node = std::move(tc);
        return ex;
    }

    // Does an uncaught `.try` failure escape this body? A TryThrow escapes
    // unless it sits inside a TryCatch's body (which catches it). Nested
    // lambdas are separate scopes, so their `.try`s don't count here.
    static auto irHasEscapingTryThrow(const ExprPtr& e) -> bool {
        if (!e) return false;
        return std::visit([](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            auto rec = [](const ExprPtr& c) { return irHasEscapingTryThrow(c); };
            if constexpr (std::is_same_v<T, TryThrow>) return true;
            else if constexpr (std::is_same_v<T, TryCatch>) {
                // n.body's throws are caught here; only clause (rescue) bodies
                // can still escape.
                for (auto& c : n.clauses) if (rec(c.body)) return true;
                return false;
            }
            else if constexpr (std::is_same_v<T, Lambda>) return false;
            else if constexpr (std::is_same_v<T, Let>) return rec(n.value) || rec(n.body);
            else if constexpr (std::is_same_v<T, Seq>) {
                for (auto& x : n.exprs) if (rec(x)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, Match>) {
                for (auto& s : n.subjects) if (rec(s)) return true;
                for (auto& c : n.clauses) {
                    if (c.guard && rec(*c.guard)) return true;
                    if (rec(c.body)) return true;
                }
                return false;
            }
            else if constexpr (std::is_same_v<T, Intrinsic>) {
                for (auto& a : n.args) if (rec(a)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, Call>) {
                for (auto& a : n.args) if (rec(a)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, CallIndirect>) {
                if (rec(n.callee)) return true;
                for (auto& a : n.args) if (rec(a)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, Construct>) {
                for (auto& a : n.args) if (rec(a)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, MakeTuple>) {
                for (auto& a : n.elements) if (rec(a)) return true; return false;
            }
            else if constexpr (std::is_same_v<T, MakeList>) {
                for (auto& a : n.elements) if (rec(a)) return true;
                return n.rest && rec(*n.rest);
            }
            else if constexpr (std::is_same_v<T, FieldGet>) return rec(n.record);
            else if constexpr (std::is_same_v<T, Return>) return rec(n.value);
            else if constexpr (std::is_same_v<T, LetRec>)
                return rec(n.funBody) || rec(n.contBody);
            return false;
        }, e->node);
    }

    // Wrap a rescue-less function body so an escaped `.try` failure becomes
    // `return Error(e)` — matching the interpreter, which catches an unhandled
    // TryException at the function boundary (evaluator.cxx execFunctionDef).
    auto wrapPropagateTryError(ExprPtr body) -> ExprPtr {
        TryCatch tc;
        tc.body = std::move(body);
        MatchClause c;
        std::string en = fresh("_TryE");
        auto p = std::make_unique<Pattern>();
        p->kind = PatKind::Var; p->name = en;
        c.patterns.push_back(std::move(p));
        auto errCtor = std::make_unique<Expr>();
        errCtor->node = Construct{"Error", one(var(en))};
        c.body = std::move(errCtor);
        tc.clauses.push_back(std::move(c));
        auto ex = std::make_unique<Expr>();
        ex->node = std::move(tc);
        return ex;
    }

    // ---- Function / program ----------------------------------------------
    // A single param → an IR pattern (var name or a destructuring pattern).
    auto lowerParam(const ast::Param& p) -> PatternPtr {
        if (p.name) {
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = *p.name;
            subst[*p.name] = *p.name;
            return pat;
        }
        if (p.pattern) return lowerPattern(*p.pattern);
        auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
    }

    // Lower a group of same-name FunctionDefs (Kex writes multi-clause
    // functions as separate `let` declarations) into one multi-clause FunDef.
    // implicitThisName: when non-empty, a receiver param of that name is
    // prepended to every clause (make-block methods: `this`).
    auto lowerFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                            const std::string& implicitThisName = "",
                            const std::string& nameOverride = "") -> FunDef {
        FunDef def;
        def.name = nameOverride.empty() ? group[0]->name : nameOverride;
        int explicitArity = group[0]->clauses.empty()
            ? 0 : static_cast<int>(group[0]->clauses[0].params.size());
        def.arity = explicitArity + (implicitThisName.empty() ? 0 : 1);
        auto savedModulePath = currentModulePath;
        for (const auto* fn : group) {
            for (const auto& clause : fn->clauses) {
                subst.clear(); // fresh scope per clause
                FunClause fc;
                if (!implicitThisName.empty()) {
                    subst[implicitThisName] = implicitThisName;
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Var; pat->name = implicitThisName;
                    fc.params.push_back(std::move(pat));
                }
                // A record-destructure or range receiver param can't be a
                // structural Core Erlang pattern (fields are read by NAME, not
                // position, and a range is a materialized list) — bind it to a
                // fresh var and prepend field/element bindings to the body.
                std::vector<std::pair<std::string, ExprPtr>> prefix;
                std::vector<std::pair<std::string, const ast::RecordPattern*>> recordPatterns;
                for (const auto& p : clause.params) {
                    if (!p.name && p.pattern) {
                        auto& pk = (*p.pattern)->kind;
                        if (auto* rp = std::get_if<ast::RecordPattern>(&pk)) {
                            std::string rv = fresh("rec");
                            auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                            fc.params.push_back(std::move(vp));
                            recordPatterns.push_back({rv, rp});
                            std::vector<std::string> names;
                            collectRecordBindings(*rp, names);
                            for (const auto& name : names) subst[name] = name;
                            continue;
                        }
                        if (auto* rgp = std::get_if<ast::RangePattern>(&pk)) {
                            std::string rv = fresh("rng");
                            auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                            fc.params.push_back(std::move(vp));
                            if (rgp->start) if (auto* sv = std::get_if<ast::VarPattern>(&rgp->start->kind))
                                prefix.push_back({sv->name, callE("erlang","hd",1,one(var(rv)))});
                            if (rgp->end) if (auto* ev = std::get_if<ast::VarPattern>(&rgp->end->kind))
                                prefix.push_back({ev->name, callE("lists","last",1,one(var(rv)))});
                            continue;
                        }
                    }
                    fc.params.push_back(lowerParam(p));
                }
                for (const auto& [nm, _] : prefix) subst[nm] = nm;
                ExprPtr body = lowerBody(clause.body);
                for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                    body = makeLet(it->first, std::move(it->second), std::move(body));
                for (auto it = recordPatterns.rbegin(); it != recordPatterns.rend(); ++it)
                    body = wrapRecordPattern(it->first, *it->second, std::move(body));
                if (clause.rescue) body = wrapWithTryCatch(std::move(body), *clause.rescue);
                else if (irHasEscapingTryThrow(body)) body = wrapPropagateTryError(std::move(body));
                fc.body = std::move(body);
                def.clauses.push_back(std::move(fc));
            }
        }
        currentModulePath = std::move(savedModulePath);
        return def;
    }
    auto lowerFunction(const ast::FunctionDef& fn, const std::string& implicitThisName = "")
        -> FunDef {
        return lowerFunctionGroup({&fn}, implicitThisName);
    }

    // ---- Records ----------------------------------------------------------
    void collectRecordLayout(const std::string& name,
                             const std::vector<std::string>& fields) {
        RecordInfo info;
        for (int i = 0; i < static_cast<int>(fields.size()); i++) {
            info.fields.push_back(fields[i]);
            info.defaults.push_back(nullptr);
            fieldAccessors[fields[i]].push_back({name, i + 2});
        }
        records[name] = std::move(info);
    }

    void collectRecord(const ast::RecordDef& rec) {
        RecordInfo info;
        for (int i = 0; i < static_cast<int>(rec.fields.size()); i++) {
            info.fields.push_back(rec.fields[i].name);
            info.defaults.push_back(rec.fields[i].defaultValue ? &*rec.fields[i].defaultValue
                                                               : nullptr);
            fieldAccessors[rec.fields[i].name].push_back({rec.name, i + 2});
        }
        records[rec.name] = std::move(info);
    }

    // Emit a `'field'/1` accessor for each record field, unless a real
    // function/method of the same name exists (which shadows it), or the
    // name matches an imported package-declared receiver function —
    // otherwise the accessor would shadow the prelude's receiver function
    // and dispatch `[1,2,3].rest` to a tuple-element read instead of
    // `kex_prelude:rest/1`. When a field sits at the same position in
    // every record that has it, one element/2 call suffices; otherwise
    // dispatch on the record tag.
    // Colliding with an IMPORTED receiver function used to suppress the field
    // accessor outright, which left `box.rest` calling the prelude's `rest/1`
    // — a dispatcher with no clause for a record, so it died with `if_clause`
    // on BEAM while the walker read the field just fine. Instead the accessor
    // is emitted and falls through to the import, so `box.rest` and
    // `[1,2,3].rest` both work.
    //
    // Returns the imported function to fall through to, or nullptr when the
    // name does not collide. Sets `blocked` when the collision cannot be
    // forwarded (no 1-arity form), in which case no accessor is emitted and
    // the name is left to the import — the old behavior. lowerProgram's
    // localMethods seeding asks the same question, so both must agree on
    // whether an accessor exists.
    auto fieldAccessorDelegate(const std::string& field, bool& blocked) const
        -> const ExternalModules::ReceiverFunction* {
        blocked = false;
        if (!externalModules) return nullptr;
        auto ext = externalModules->receiverFunctions.find(field);
        if (ext == externalModules->receiverFunctions.end()) return nullptr;
        for (const auto& candidate : ext->second)
            if (candidate.beamArity == 1) return &candidate;
        // Every import of this name takes more than a receiver, so there is
        // nothing to collide with: BEAM keys functions by name AND arity, and
        // a field read is always arity 1. `Kex.Kernel.VERSION.patch` is the
        // case — `patch` is also `Http.patch/2,3`, and blocking the accessor
        // here left the field unreadable on BEAM while the walker read it
        // fine. Emitting `patch/1` cannot shadow `patch/2`.
        return nullptr;
    }

    auto makeAccessors(const std::unordered_set<std::string>& definedFns) -> std::vector<FunDef> {
        std::vector<FunDef> out;
        for (const auto& [field, entries] : fieldAccessors) {
            // A local function of this name owns the symbol; its dispatcher
            // carries the record clauses instead (see appendFieldClauses).
            if (definedFns.count(field)) continue;

            bool blocked = false;
            const auto* delegate = fieldAccessorDelegate(field, blocked);
            if (blocked) continue;

            FunDef def; def.name = field; def.arity = 1;
            FunClause fc;
            auto rp = std::make_unique<Pattern>(); rp->kind = PatKind::Var; rp->name = "_rec";
            fc.params.push_back(std::move(rp));
            bool allSame = std::all_of(entries.begin(), entries.end(),
                [&](const auto& e){ return e.second == entries[0].second; });
            if (delegate) {
                // Match the record tags this field belongs to; anything else
                // is the imported function's receiver.
                Match m;
                m.subjects.push_back(var("_rec"));
                for (const auto& [recName, pos] : entries) {
                    MatchClause mc;
                    auto gv = std::make_unique<Pattern>();
                    gv->kind = PatKind::Var; gv->name = "_fv";
                    mc.patterns.push_back(std::move(gv));
                    mc.guard = typeGuard(recName, var("_fv"));
                    mc.body = callE("erlang", "element", 2, two(litInt(pos), var("_rec")));
                    m.clauses.push_back(std::move(mc));
                }
                MatchClause fallback;
                auto wild = std::make_unique<Pattern>();
                wild->kind = PatKind::Var; wild->name = "_other";
                fallback.patterns.push_back(std::move(wild));
                fallback.body = callE(delegate->moduleAtom, delegate->beamFunction,
                                      1, one(var("_rec")));
                m.clauses.push_back(std::move(fallback));
                auto e = std::make_unique<Expr>(); e->node = std::move(m);
                fc.body = std::move(e);
            } else if (allSame) {
                fc.body = callE("erlang", "element", 2, two(litInt(entries[0].second), var("_rec")));
            } else {
                Match m;
                m.subjects.push_back(callE("erlang", "element", 2, two(litInt(1), var("_rec"))));
                for (const auto& [recName, pos] : entries) {
                    MatchClause mc;
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Lit; p->litKind = LitKind::Atom; p->litText = recName;
                    mc.patterns.push_back(std::move(p));
                    mc.body = callE("erlang", "element", 2, two(litInt(pos), var("_rec")));
                    m.clauses.push_back(std::move(mc));
                }
                auto e = std::make_unique<Expr>(); e->node = std::move(m);
                fc.body = std::move(e);
            }
            def.clauses.push_back(std::move(fc));
            out.push_back(std::move(def));
        }
        return out;
    }

    // ---- Make blocks ------------------------------------------------------
    // Lower one make-block method to a FunDef. First cut: implicit-`this`
    // methods with named/simple params (the common `@field`/`this.method`
    // shape). Static constructors and receiver-pattern methods are deferred.
    // Lower a group of same-name make-block methods (multi-clause) for one
    // type. A method whose first param is an unnamed pattern (`@Less`,
    // `{input, pos}`, `@[x|_]`) matches the RECEIVER directly — no implicit
    // `this`. Otherwise `this` is prepended.
    auto lowerMakeGroup(const std::vector<const ast::FunctionDef*>& group,
                        const std::string& typeName) -> FunDef {
        const auto& first = *group[0];
        bool isStaticCtor = !first.name.empty()
                          && std::isupper(static_cast<unsigned char>(first.name[0]));
        if (isStaticCtor)
            throw LowerError("IR lower: static constructor '" + first.name + "' not yet ported");
        // A receiver pattern is specifically the `@` sigil (ThisPattern), a
        // record destructure (RecordPattern), or a range (RangePattern) —
        // NOT any pattern. A bare type-name param like `to(String)` is a
        // ConstructorPattern *value* argument, and such a method still uses
        // its implicit `this` (spec/type_dispatch.kex's `to(String)` body is
        // `"(${@x}, ${@y})"`).
        bool receiverPattern = false;
        if (!first.clauses.empty() && !first.clauses[0].params.empty()) {
            const auto& p0 = first.clauses[0].params[0];
            if (!p0.name && p0.pattern)
                receiverPattern =
                    std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
                    std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
                    std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind);
        }
        if (!receiverPattern) implicitThisMethods.insert(first.name);
        // A make-String method matching the receiver with LIST patterns
        // (`myCapitalize(@[x | rest])`) — legal, since [Char] IS String —
        // can't match a binary receiver directly. Detect it so the whole
        // group gets wrapped in an as_list coercion below.
        bool listReceiver = false;
        if (receiverPattern && typeName == "String")
            for (const auto* fn : group)
                for (const auto& cl : fn->clauses)
                    if (!cl.params.empty() && !cl.params[0].name && cl.params[0].pattern)
                        if (auto* tp = std::get_if<ast::ThisPattern>(&(*cl.params[0].pattern)->kind))
                            if (tp->inner && std::holds_alternative<ast::ListPattern>(tp->inner->kind))
                                listReceiver = true;
        auto previousMakeType = currentMakeType;
        currentMakeType = typeName;
        auto def = lowerFunctionGroup(group, receiverPattern ? "" : "this");
        currentMakeType = std::move(previousMakeType);
        // An implicit method on a record still needs to constrain its receiver
        // in Core Erlang. Without this guard, a method such as
        // `Measure.to(String)` becomes a catch-all `to/2` clause and shadows
        // the universal conversion function for every other value.
        if (!receiverPattern && records.count(typeName))
            for (auto& clause : def.clauses)
                clause.guard = typeGuard(typeName, var("this"));
        if (listReceiver) def = coerceListReceiver(std::move(def));
        if (argumentOverloadedMethods.count(
                localOverloadKey(first.name, typeName, def.arity)))
            def.name = mangleReceiverSignature(
                first.name, methodDispatchTypes(first, typeName));
        else if (collidingMethods.count(first.name) && !typeName.empty())
            def.name = mangleReceiverImplementation(first.name, typeName);
        return def;
    }

    // Rewrap a FunDef whose clauses pattern-match the receiver as a list:
    // one wrapper clause binds raw params, then a multi-subject Match runs
    // the original clause patterns against `as_list(receiver)` (a String
    // binary becomes its [Char] codepoint list; lists pass through).
    auto coerceListReceiver(FunDef def) -> FunDef {
        FunDef out; out.name = def.name; out.arity = def.arity;
        FunClause fc;
        Match m;
        for (int i = 0; i < def.arity; i++) {
            std::string p = "_cr" + std::to_string(i);
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = p;
            fc.params.push_back(std::move(pat));
            m.subjects.push_back(i == 0
                ? callE("kex_intrinsic_list", "as_list", 1, one(var(p)))
                : var(p));
        }
        for (auto& cl : def.clauses) {
            MatchClause mc;
            mc.patterns = std::move(cl.params);
            mc.guard = std::move(cl.guard);
            mc.body = std::move(cl.body);
            m.clauses.push_back(std::move(mc));
        }
        auto b = std::make_unique<Expr>();
        b->node = std::move(m);
        fc.body = std::move(b);
        out.clauses.push_back(std::move(fc));
        return out;
    }

    static auto primGuardBifs() -> const std::unordered_map<std::string, const char*>& {
        static const std::unordered_map<std::string, const char*> m = {
            {"Integer","is_integer"}, {"Float","is_float"},
            {"Number","is_number"}, {"String","is_binary"}, {"Bool","is_boolean"},
            {"Map","is_map"}, {"List","is_list"},
            {"Range","is_list"},
            {"Pid","is_pid"}, {"Task","is_pid"}, {"Reference","is_reference"},
        };
        return m;
    }

    auto typeGuard(const std::string& ty, ExprPtr v) -> ExprPtr {
        auto it = primGuardBifs().find(ty);
        if (it != primGuardBifs().end())
            return callE("erlang", it->second, 1, one(std::move(v)));
        // Tagged record. Core Erlang exposes the guard-safe is_record/3 BIF
        // (the source-level is_record/2 form is a compiler macro).
        if (auto record = records.find(ty); record != records.end())
            return callE("erlang", "is_record", 3,
                         three(std::move(v), lit(LitKind::Atom, ty),
                               litInt(static_cast<int64_t>(
                                   record->second.fields.size() + 1))));
        // Unknown tagged type fallback.
        auto vRef = snap(v);
        return callE("erlang", "and", 2, two(
            callE("erlang", "is_tuple", 1, one(vRef.get())),
            intrin(Op::Eq, two(callE("erlang", "element", 2,
                                     two(litInt(1), vRef.get())),
                               lit(LitKind::Atom, ty)))));
    }

    auto makeArgumentDispatcher(
        const std::string& name, int arity,
        const std::vector<std::pair<std::string, std::vector<std::string>>>&
            overloads) -> FunDef {
        FunDef def;
        def.name = name;
        def.arity = arity;
        for (const auto& [target, types] : overloads) {
            if (static_cast<int>(types.size()) != arity) continue;
            FunClause clause;
            std::vector<ExprPtr> forwarded;
            ExprPtr guard;
            for (int i = 0; i < arity; ++i) {
                auto param = std::make_unique<Pattern>();
                param->kind = PatKind::Var;
                param->name = "_a" + std::to_string(i);
                clause.params.push_back(std::move(param));
                forwarded.push_back(var("_a" + std::to_string(i)));
                auto next = typeGuard(
                    types[static_cast<size_t>(i)],
                    var("_a" + std::to_string(i)));
                guard = guard
                    ? callE("erlang", "and", 2,
                            two(std::move(guard), std::move(next)))
                    : std::move(next);
            }
            clause.guard = std::move(guard);
            auto body = std::make_unique<Expr>();
            body->node = Call{"", target, arity, std::move(forwarded), false};
            clause.body = std::move(body);
            def.clauses.push_back(std::move(clause));
        }
        return def;
    }

    auto makeDispatcher(const std::string& name, int arity,
                        const std::vector<std::string>& owners) -> FunDef {
        FunDef def; def.name = name; def.arity = arity;
        FunClause fc;
        std::vector<ExprPtr> fwdArgs;
        for (int i = 0; i < arity; i++) {
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = "_a" + std::to_string(i);
            fc.params.push_back(std::move(pat));
            fwdArgs.push_back(var("_a" + std::to_string(i)));
        }
        // Dispatch on the receiver's runtime type via a guard, so it works for
        // both tagged records/variants AND primitive types (Integer/Float/Map/
        // String/…), whose values aren't tagged tuples — `element(1, N)` on a
        // raw number would crash. For a record type T the guard is
        // `is_tuple(V) and element(1,V) == 'T'`; for a primitive it's the
        // matching `is_*` BIF (all guard-safe).
        Match m;
        m.subjects.push_back(var("_a0"));
        // Primitive-type guards (is_list, is_binary, …) are always safe.
        // Record/variant guards use erlang:and(is_tuple(V), element(1,V)==Tag)
        // which evaluates element/2 eagerly and crashes on non-tuples. Emit
        // safe clauses first so a primitive receiver matches before reaching
        // an element-based guard.
        std::vector<std::string> sortedOwners;
        std::vector<std::string> deferredOwners;
        std::unordered_set<std::string> seen;
        for (const auto& ty : owners) {
            if (!seen.insert(ty).second) continue;
            if (primGuardBifs().count(ty) || ty == "Char"
                || (typeVariantTags.count(ty) && !typeVariantTags.at(ty).empty()))
                sortedOwners.push_back(ty);
            else
                deferredOwners.push_back(ty);
        }
        // When two types share the same BIF guard, the first clause
        // shadows the second.  Keep the more general type and drop the
        // shadowed one — its implementation is reachable through the
        // general type's method anyway (e.g. Range methods materialise
        // via .items and re-enter as a List).
        auto dropShadowed = [&](const std::string& keep,
                                const std::string& drop) {
            if (std::find(sortedOwners.begin(), sortedOwners.end(), keep) != sortedOwners.end()) {
                auto it = std::find(sortedOwners.begin(), sortedOwners.end(), drop);
                if (it != sortedOwners.end()) sortedOwners.erase(it);
            }
        };
        dropShadowed("List", "Range");
        dropShadowed("Task", "Pid");
        sortedOwners.insert(sortedOwners.end(), deferredOwners.begin(), deferredOwners.end());
        for (const auto& ty : sortedOwners) {
            // ADT types with known variant tags: generate one pattern-match
            // clause per variant instead of a single guard clause. This avoids
            // Core Erlang's strict erlang:and/or in guards (element/2 throws
            // badarg on non-tuples) and uses the correct variant tags rather
            // than the type name (e.g. 'Just' not 'Optional').
            auto vt = typeVariantTags.find(ty);
            if (vt != typeVariantTags.end() && !vt->second.empty()) {
                for (const auto& tag : vt->second) {
                    MatchClause mc;
                    // Nullary variant (no payload) → bare atom on BEAM.
                    if (nullaryVariantTags.count(tag)) {
                        auto pat = std::make_unique<Pattern>();
                        pat->kind = PatKind::Lit;
                        pat->litKind = LitKind::Atom;
                        pat->litText = tag;
                        mc.patterns.push_back(std::move(pat));
                    } else {
                        // Payload variant: tuple pattern {Tag, _}.
                        auto pat = std::make_unique<Pattern>();
                        pat->kind = PatKind::Tuple;
                        auto tagPat = std::make_unique<Pattern>();
                        tagPat->kind = PatKind::Lit;
                        tagPat->litKind = LitKind::Atom;
                        tagPat->litText = tag;
                        pat->args.push_back(std::move(tagPat));
                        auto wild = std::make_unique<Pattern>();
                        wild->kind = PatKind::Wild;
                        pat->args.push_back(std::move(wild));
                        mc.patterns.push_back(std::move(pat));
                    }
                    std::vector<ExprPtr> args;
                    for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                    auto call = std::make_unique<Expr>();
                    call->node = Call{"",
                        mangleReceiverImplementation(name, ty),
                        arity, std::move(args), false};
                    mc.body = std::move(call);
                    m.clauses.push_back(std::move(mc));
                }
            } else if (ty == "Char") {
                // A Char is always the 2-tuple {'Char', _} — match it
                // structurally. (An element(1,_) guard would badarg, not
                // soft-fail, for a primitive receiver reaching this clause.)
                MatchClause mc;
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Tuple;
                auto tagPat = std::make_unique<Pattern>();
                tagPat->kind = PatKind::Lit;
                tagPat->litKind = LitKind::Atom;
                tagPat->litText = "Char";
                pat->args.push_back(std::move(tagPat));
                auto wild = std::make_unique<Pattern>();
                wild->kind = PatKind::Wild;
                pat->args.push_back(std::move(wild));
                mc.patterns.push_back(std::move(pat));
                std::vector<ExprPtr> args;
                for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                auto call = std::make_unique<Expr>();
                call->node = Call{"",
                    mangleReceiverImplementation(name, ty),
                    arity, std::move(args), false};
                mc.body = std::move(call);
                m.clauses.push_back(std::move(mc));
            } else {
                // Primitive/record type: use a guard-based clause.
                MatchClause mc;
                auto gv = std::make_unique<Pattern>();
                gv->kind = PatKind::Var; gv->name = "_gv";
                mc.patterns.push_back(std::move(gv));
                mc.guard = typeGuard(ty, var("_gv"));
                std::vector<ExprPtr> args;
                for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                auto call = std::make_unique<Expr>();
                call->node = Call{"",
                    mangleReceiverImplementation(name, ty),
                    arity, std::move(args), false};
                mc.body = std::move(call);
                m.clauses.push_back(std::move(mc));
            }
        }
        // [Char] IS String: a method owned by List but not by String must
        // still take a String receiver — coerce the binary to its codepoint
        // list and forward to the List impl. This clause goes FIRST: the
        // record/variant owner clauses guard with element(1, _) which erlc
        // does not soft-fail for a binary falling through to them.
        bool hasString = std::find(owners.begin(), owners.end(), "String") != owners.end();
        bool hasList = std::find(owners.begin(), owners.end(), "List") != owners.end();
        if (hasList && !hasString) {
            MatchClause mc;
            auto gv = std::make_unique<Pattern>();
            gv->kind = PatKind::Var; gv->name = "_gv";
            mc.patterns.push_back(std::move(gv));
            mc.guard = callE("erlang", "is_binary", 1, one(var("_gv")));
            std::vector<ExprPtr> args;
            args.push_back(callE("kex_intrinsic_list", "as_list", 1, one(var("_a0"))));
            for (int i = 1; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
            auto call = std::make_unique<Expr>();
            call->node = Call{"",
                mangleReceiverImplementation(name, "List"),
                arity, std::move(args), false};
            mc.body = std::move(call);
            m.clauses.insert(m.clauses.begin(), std::move(mc));
        }
        appendFieldClauses(name, arity, m);
        // A receiver no clause covers is a call that should not have compiled
        // — `Date.of(...).iso`, where `iso` belongs to Date and the receiver
        // is a Result. The checker rejects that now, but `--no-check` and
        // gradual code still reach here, and falling off the end raised a
        // bare `if_clause`: no method name, no receiver, no location. Name it
        // instead, in the walker's wording, with the receiver's runtime type.
        {
            MatchClause fallback;
            auto wild = std::make_unique<Pattern>();
            wild->kind = PatKind::Wild;
            fallback.patterns.push_back(std::move(wild));
            auto message = callE("erlang", "iolist_to_binary", 1,
                one(makeListOf(
                    lit(LitKind::String, "runtime error: Undefined method: " +
                                          name + " for "),
                    callE("kex_io", "value_type_name", 1, one(var("_a0"))))));
            fallback.body = callE("erlang", "error", 1, one(std::move(message)));
            m.clauses.push_back(std::move(fallback));
        }
        auto body = std::make_unique<Expr>();
        body->node = std::move(m);
        fc.body = std::move(body);
        def.clauses.push_back(std::move(fc));
        return def;
    }

    // [a, b] as an IR list, for building an iolist at runtime.
    auto makeListOf(ExprPtr first, ExprPtr second) -> ExprPtr {
        std::vector<ExprPtr> elements;
        elements.push_back(std::move(first));
        elements.push_back(std::move(second));
        auto list = std::make_unique<Expr>();
        list->node = MakeList{std::move(elements), std::nullopt};
        return list;
    }

    // A record field can share its name with a receiver method — `rest` is a
    // field of the prelude's ParseError and a method on List/String. The
    // standalone `'rest'/1` accessor is suppressed in that case (makeAccessors)
    // because it would shadow the method and send `[1,2,3].rest` to a tuple
    // read, which leaves the dispatcher as the only `rest/1` in the module.
    // Without these clauses it has no arm matching a record, so `err.rest`
    // died with `if_clause` on BEAM while working fine in the walker.
    //
    // Appended last so a genuine method for the receiver's type always wins;
    // these only catch records that would otherwise fall off the end.
    auto appendFieldClauses(const std::string& name, int arity, Match& m) -> void {
        if (arity != 1) return;
        auto it = fieldAccessors.find(name);
        if (it == fieldAccessors.end()) return;
        for (const auto& [recordName, position] : it->second) {
            MatchClause mc;
            auto gv = std::make_unique<Pattern>();
            gv->kind = PatKind::Var; gv->name = "_fv";
            mc.patterns.push_back(std::move(gv));
            mc.guard = typeGuard(recordName, var("_fv"));
            mc.body = callE("erlang", "element", 2,
                            two(litInt(position), var("_a0")));
            m.clauses.push_back(std::move(mc));
        }
    }
};

// A make-block method's BEAM arity: the AST parameter count, plus 1 for the
// implicit `this` UNLESS the first param is itself the receiver (an `@`/record/
// range pattern). So `count(@[])` is count/1 but `count(pred)` is count/2 even
// though both have one AST param. Grouping and trait-inheritance both key on
// this — a method can be overloaded by BEAM arity across those two forms.
// Every name a pattern binds, in source order.
static auto patternBoundNames(const ast::PatternPtr& p,
                              std::vector<std::string>& out) -> void {
    if (!p) return;
    std::visit([&](const auto& pn) {
        using T = std::decay_t<decltype(pn)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (pn.name != "_") out.push_back(pn.name);
        } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& a : pn.args) patternBoundNames(a, out);
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& a : pn.elements) patternBoundNames(a, out);
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& a : pn.elements) patternBoundNames(a, out);
            if (pn.rest) patternBoundNames(*pn.rest, out);
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            // A field with no sub-pattern is the shorthand binding.
            for (const auto& f : pn.fields) {
                if (f.pattern) patternBoundNames(*f.pattern, out);
                else if (f.name != "_") out.push_back(f.name);
            }
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (pn.inner) patternBoundNames(pn.inner, out);
        }
    }, p->kind);
}

static auto beamArity(const ast::FunctionDef* fd) -> size_t {
    if (!fd || fd->clauses.empty()) return 1;
    const auto& params = fd->clauses[0].params;
    if (params.empty()) return 1; // implicit-this only, e.g. `let reverse = …`
    const auto& p0 = params[0];
    bool receiverPat = !p0.name && p0.pattern &&
        (std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind));
    return receiverPat ? params.size() : params.size() + 1;
}

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::string& sourcePath,
                  const ExternalModules* externals,
                  const std::vector<ExternalRecordLayout>* externalRecords,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls,
                  bool preferExternalReceivers,
                  const std::vector<ExternalVariantTag>* externalVariants,
                  const StaticTypeOfCalls* staticTypeOfCalls)
    -> Module {
    Lowering L;
    L.sourceFile = sourcePath;
    L.externalModules = externals;
    L.resolvedCalls = resolvedCalls;
    L.preferExternalReceivers = preferExternalReceivers;
    if (externals)
        for (const auto& [name, candidates] : externals->receiverFunctions) {
            if (name.empty() ||
                (std::isalnum(static_cast<unsigned char>(name[0])) ||
                 name[0] == '_'))
                continue;
            if (std::any_of(candidates.begin(), candidates.end(),
                            [](const auto& candidate) {
                                return candidate.beamArity == 2;
                            }))
                L.overloadedOps.insert(name);
        }
    Module mod;
    mod.name = "kex_" + fileStem;

    // Traits: a `make Type, implement: T do ... end` block inherits every
    // default method (a trait `let m = ...` with a body) of each T it doesn't
    // override itself. We splice those inherited defaults into the type's
    // method group so they go through the exact same lowering/dispatch as
    // directly-defined methods (spec/traits.kex: Bot inherits shout/passing?).
    std::unordered_map<std::string, const ast::TraitDef*> traitDefs;
    for (const auto& item : prog.items)
        if (auto* td = std::get_if<std::unique_ptr<ast::TraitDef>>(&item); td && *td)
            traitDefs[(*td)->name] = td->get();
    // Keyed by (name, BEAM arity): a type overrides a trait default only when it
    // defines the same name at the SAME arity (list.kex's count/1 must NOT block
    // inheriting Enumerable's count/2).
    using MethodKey = std::pair<std::string, size_t>;
    auto ownMethodNames = [](const ast::MakeDef& mk) {
        std::set<MethodKey> s;
        auto add = [&](const ast::FunctionDef* f){ if (f) s.insert({f->name, beamArity(f)}); };
        for (const auto& bi : mk.body) {
            if (auto* f = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) add(f->get());
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                if (*vb) for (const auto& vi : (*vb)->items)
                    if (auto* vf = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi)) add(vf->get());
        }
        return s;
    };
    auto inheritedDefaults = [&](const ast::MakeDef& mk) {
        std::vector<const ast::FunctionDef*> out;
        if (mk.implements.empty()) return out;
        auto own = ownMethodNames(mk);
        std::set<MethodKey> added;
        for (const auto& tn : mk.implements) {
            auto it = traitDefs.find(tn);
            if (it == traitDefs.end()) continue;
            for (const auto& ti : it->second->body) {
                auto* f = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ti);
                if (!f || !*f) continue;
                MethodKey key{(*f)->name, beamArity(f->get())};
                if (own.count(key) || added.count(key)) continue;
                // Only default methods (with a body) are inherited; a bare
                // signature `describe : () -> String` carries no clause body.
                if ((*f)->clauses.empty() || (*f)->clauses[0].body.empty()) continue;
                out.push_back(f->get()); added.insert(key);
            }
        }
        return out;
    };

    // Pre-pass: collect record layouts (needed for construction/field access
    // before any body is lowered) and the set of real function/method names
    // (so field accessors that would collide with them are suppressed).
    // Factored into per-kind lambdas so the flattened module items go
    // through the exact same collection as top-level items.
    std::unordered_set<std::string> definedFns;
    std::unordered_set<std::string> staticMethodNames; // record static blocks
    auto preRecord = [&](const ast::RecordDef& rd) {
        L.collectRecord(rd);
        L.knownTypes.insert(rd.name);
        if (rd.staticBlock)
            for (const auto& sf : rd.staticBlock->functions)
                if (sf) {
                    definedFns.insert(
                        mangleQualifiedMember(rd.name, sf->name));
                    staticMethodNames.insert(sf->name);
                }
    };
    if (externalRecords)
        for (const auto& record : *externalRecords) {
            L.collectRecordLayout(record.name, record.fields);
            L.knownTypes.insert(record.name);
        }
    // Display info only — deliberately NOT added to variantTagSet, which
    // drives codegen decisions for tags this module declares itself.
    L.staticTypeOfCalls = staticTypeOfCalls;
    if (externalVariants)
        for (const auto& variant : *externalVariants) {
            L.variantArity[variant.tag] = variant.arity;
            L.variantOwner[variant.tag] = variant.owner;
        }
    auto preFn = [&](const ast::FunctionDef& fd) {
        definedFns.insert(fd.name);
        if (!fd.clauses.empty())
            L.localMethodArities.insert(
                fd.name + "/" + std::to_string(fd.clauses[0].params.size()));
        if (!fd.clauses.empty()) {
            std::vector<std::string> pnames;
            for (const auto& p : fd.clauses[0].params)
                pnames.push_back(p.name ? *p.name : "");
            if (pnames.empty()) L.zeroArgFns.insert(fd.name);
            L.fnParamNames[fd.name] = std::move(pnames);
        }
    };
    auto preType = [&](const ast::TypeDef& td) {
        if (!td.variants) return;
        if (kex::isTransparentTypeAlias(td)) return;
        for (const auto& v : *td.variants) {
            auto t = Lowering::simpleTypeName(v);
            if (t.empty()) continue;
            L.variantTagSet.insert(t);
            if (auto* g = std::get_if<ast::GenericType>(&v->kind))
                L.variantArity[t] = static_cast<int>(g->args.size());
            else
                L.variantArity[t] = 0;
            L.variantOwner[t] = td.name;
        }
    };
    auto moduleConstructorDef =
        [&](const kex::TypeConstructorInfo& constructor,
            const std::string& modulePath) {
            FunDef def;
            def.name =
                mangleModuleMember(modulePath, constructor.name);
            def.arity = static_cast<int>(constructor.arity);
            FunClause clause;
            std::vector<ExprPtr> args;
            for (size_t i = 0; i < constructor.arity; ++i) {
                const auto name = "_constructor_arg_" +
                    std::to_string(i);
                auto param = std::make_unique<Pattern>();
                param->kind = PatKind::Var;
                param->name = name;
                clause.params.push_back(std::move(param));
                args.push_back(var(name));
            }
            auto body = std::make_unique<Expr>();
            body->node =
                Construct{constructor.name, std::move(args)};
            clause.body = std::move(body);
            def.clauses.push_back(std::move(clause));
            return def;
        };
    auto preMake = [&](const ast::MakeDef& md) {
        std::string typeName = Lowering::simpleTypeName(md.target);
        std::unordered_map<std::string,
            std::map<std::string, std::vector<std::string>>>
            signaturesByNameAndArity;
        auto collectMethod = [&](const ast::FunctionDef* fd) {
            if (!fd) return;
            definedFns.insert(fd->name);
            if (!fd->clauses.empty()) {
                // A make-block method reaches its receiver one of two ways:
                // implicitly through `this` (params exclude it), or as its
                // first pattern parameter (`let rangeStart(x.._)`,
                // `let startsWith_(@['_' | rest])`). Both spellings are legal,
                // and which one is in play is not decidable here, so record
                // both candidate arities — the guard's job is only to reject
                // an arity that exists NOWHERE locally.
                const auto params = fd->clauses[0].params.size();
                L.localMethodArities.insert(fd->name + "/" + std::to_string(params));
                L.localMethodArities.insert(fd->name + "/" + std::to_string(params + 1));
                auto dispatchTypes = methodDispatchTypes(*fd, typeName);
                const auto signature =
                    mangleReceiverSignature(fd->name, dispatchTypes);
                signaturesByNameAndArity[
                    fd->name + "/" + std::to_string(beamArity(fd))]
                        .emplace(signature, std::move(dispatchTypes));
            }
            const std::string& mn = fd->name;
            if (!mn.empty() && !std::isalnum(static_cast<unsigned char>(mn[0])) && mn[0] != '_')
                L.overloadedOps.insert(mn);
            if (!typeName.empty()) {
                auto& owners = L.methodOwners[fd->name];
                if (std::find(owners.begin(), owners.end(), typeName) == owners.end())
                    owners.push_back(typeName);
            }
            if (!fd->clauses.empty()) {
                std::vector<const ast::ExprPtr*> defaults;
                for (const auto& p : fd->clauses[0].params)
                    defaults.push_back(p.defaultValue ? &*p.defaultValue : nullptr);
                if (std::any_of(defaults.begin(), defaults.end(), [](auto* d) { return d != nullptr; }))
                    L.methodDefaults[fd->name] = std::move(defaults);
            }
        };
        for (const auto& bi : md.body) {
            if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                collectMethod(mfd->get());
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                if (*vb) for (const auto& vi : (*vb)->items)
                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                        collectMethod(vfd->get());
        }
        // Inherited trait defaults count as this type's methods too.
        for (const auto* fd : inheritedDefaults(md)) collectMethod(fd);
        for (const auto& [nameAndArity, signatures] : signaturesByNameAndArity) {
            if (signatures.size() < 2) continue;
            auto slash = nameAndArity.rfind('/');
            if (slash == std::string::npos) continue;
            const auto method = nameAndArity.substr(0, slash);
            const auto arity = static_cast<size_t>(
                std::stoul(nameAndArity.substr(slash + 1)));
            L.argumentOverloadedMethods.insert(
                localOverloadKey(method, typeName, arity));
            auto& recorded = L.argumentOverloadSignatures[
                localOverloadKey(method, typeName, arity)];
            for (const auto& [signature, dispatchTypes] : signatures) {
                definedFns.insert(signature);
                recorded.push_back({signature, dispatchTypes});
            }
        }
    };
    std::function<void(const ast::ModuleDef&)> preModule;
    preModule = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        auto preModuleFn = [&](const ast::FunctionDef* fd) {
            if (!fd) return;
            const std::string emitted = mangleModuleMember(path, fd->name);
            definedFns.insert(emitted);
            L.moduleFunctions[path + "." + fd->name] = emitted;
            if (!fd->clauses.empty()) {
                std::vector<std::string> pnames;
                for (const auto& p : fd->clauses[0].params)
                    pnames.push_back(p.name ? *p.name : "");
                L.fnParamNames[path + "." + fd->name] = std::move(pnames);
            }
        };
        auto preModuleType = [&](const ast::TypeDef* td) {
            if (!td) return;
            preType(*td);
            if (auto constructors = kex::typeConstructors(*td))
                for (const auto& constructor : *constructors) {
                    const auto emitted =
                        mangleModuleMember(path, constructor.name);
                    definedFns.insert(emitted);
                    L.moduleFunctions[path + "." +
                                      constructor.name] = emitted;
                    L.fnParamNames[path + "." +
                                   constructor.name] =
                        std::vector<std::string>(
                            constructor.arity);
                }
        };
        for (const auto& item : module.body) {
            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                preModuleFn(fd->get());
            else if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                if (*rd) preRecord(**rd);
            } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*md) preMake(**md);
            } else if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
                preModuleType(td->get());
            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item)) {
                if (*cb) for (const auto& ci : (*cb)->items) {
                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci))
                        preModuleFn(cfd->get());
                    else if (auto* crd = std::get_if<std::unique_ptr<ast::RecordDef>>(&ci)) {
                        if (*crd) preRecord(**crd);
                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                        if (*cmd) preMake(**cmd);
                    } else if (auto* ctd = std::get_if<std::unique_ptr<ast::TypeDef>>(&ci)) {
                        if (*ctd) preType(**ctd);
                    }
                }
            }
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                if (*vb) for (const auto& vi : (*vb)->items) {
                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                        preModuleFn(vfd->get());
                    else if (auto* vtd =
                                 std::get_if<std::unique_ptr<ast::TypeDef>>(
                                     &vi))
                        preModuleType(vtd->get());
                }
            } else if (auto* ed = std::get_if<std::unique_ptr<ast::ExportDecl>>(&item)) {
                if (*ed) {
                    std::string srcMod;
                    for (size_t i = 0; i < (*ed)->module.parts.size(); i++) {
                        if (i) srcMod += ".";
                        srcMod += (*ed)->module.parts[i];
                    }
                    auto alias = (*ed)->alias.value_or((*ed)->module.parts.back());
                    auto nsPath = path + "." + alias;
                    for (const auto& [key, val] : L.moduleFunctions) {
                        if (key.rfind(srcMod + ".", 0) != 0) continue;
                        auto bare = key.substr(srcMod.size() + 1);
                        if (bare.find('.') != std::string::npos) continue;
                        if (!(*ed)->onlyNames.empty()
                            && std::find((*ed)->onlyNames.begin(), (*ed)->onlyNames.end(), bare)
                                == (*ed)->onlyNames.end()) continue;
                        if (std::find((*ed)->exceptNames.begin(), (*ed)->exceptNames.end(), bare)
                            != (*ed)->exceptNames.end()) continue;
                        L.moduleFunctions[nsPath + "." + bare] = val;
                    }
                }
            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*child) preModule(**child);
            }
        }
    };
    for (const auto& item : prog.items) {
        if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
            if (*rd) preRecord(**rd);
        } else if (auto* mb = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
            if (*mb && (*mb)->synthetic)
                for (const auto& e : (*mb)->body)
                    if (auto* le = std::get_if<ast::LetExpr>(&e->kind))
                        if (le->pattern) {
                            // Every name, so a destructuring top-level `let`
                            // registers all of them and not just a bare one.
                            std::vector<std::string> names;
                            patternBoundNames(le->pattern, names);
                            for (auto& name : names)
                                L.topLevelConstants.insert(std::move(name));
                        }
        } else if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fd) preFn(**fd);
        } else if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
            if (*td) preType(**td);
        } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
            if (*md) preMake(**md);
        } else if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
            if (*module) preModule(**module);
        }
    }
    for (const auto& [name, owners] : L.methodOwners)
        if (owners.size() > 1 || L.fieldAccessors.count(name))
            L.collidingMethods.insert(name);
    // A `.method` may use the local-apply UFCS fallback iff it names a real
    // local function or a record field accessor.
    L.knownFns = definedFns;
    L.localMethods = definedFns;
    L.staticCtorNames = staticMethodNames;
    for (const auto& n : staticMethodNames) L.localMethods.insert(n);
    // Record field accessors participate in localMethods only when an
    // accessor is actually emitted for them — a field whose name collides
    // with an imported package-declared receiver function is skipped (see
    // makeAccessors) so it must not appear as a local method either.
    for (const auto& [field, entries] : L.fieldAccessors) {
        (void)entries;
        bool blocked = false;
        L.fieldAccessorDelegate(field, blocked);
        if (blocked) continue;
        L.localMethods.insert(field);
        L.localMethodArities.insert(field + "/1");
    }

    // Buffer consecutive same-name top-level functions so they group into one
    // multi-clause FunDef (flushed on any other item / name change / end).
    std::vector<const ast::FunctionDef*> fnGroup;
    auto flushGroup = [&]() {
        if (!fnGroup.empty()) { mod.functions.push_back(L.lowerFunctionGroup(fnGroup)); fnGroup.clear(); }
    };
    // Bare top-level expressions (no explicit `main`) → one synthetic main/0.
    std::vector<const ast::ExprPtr*> bareExprs;

    for (const auto& item : prog.items) {
        if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fdp && *fdp) {
            if (!fnGroup.empty() && (fnGroup.front()->name != (*fdp)->name || beamArity(fnGroup.front()) != beamArity(fdp->get()))) flushGroup();
            fnGroup.push_back(fdp->get());
            continue;
        }
        flushGroup();
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                (void)node; // handled above
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                // Layout already collected; accessors emitted after the loop.
                // Record static-block functions emit as top-level functions so
                // `RecordName.method(...)` namespace calls find them.
                if (node && node->staticBlock) {
                    for (const auto& sf : node->staticBlock->functions) {
                        if (!sf) continue;
                        std::vector<const ast::FunctionDef*> tmp{sf.get()};
                        std::string mangled =
                            mangleQualifiedMember(node->name, sf->name);
                        mod.functions.push_back(L.lowerFunctionGroup(tmp, "", mangled));
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (!node) return;
                std::string typeName = Lowering::simpleTypeName(node->target);
                std::vector<const ast::FunctionDef*> mgrp;
                auto flushM = [&]{ if (!mgrp.empty()) { mod.functions.push_back(L.lowerMakeGroup(mgrp, typeName)); mgrp.clear(); } };
                auto pushFn = [&](const ast::FunctionDef* fd) {
                    if (!fd) return;
                    bool differentOverload = false;
                    if (!mgrp.empty() &&
                        L.argumentOverloadedMethods.count(localOverloadKey(
                            fd->name, typeName, beamArity(fd)))) {
                        differentOverload =
                            methodDispatchTypes(*mgrp.front(), typeName) !=
                            methodDispatchTypes(*fd, typeName);
                    }
                    if (!mgrp.empty() &&
                        (mgrp.front()->name != fd->name ||
                         beamArity(mgrp.front()) != beamArity(fd) ||
                         differentOverload))
                        flushM();
                    mgrp.push_back(fd);
                };
                for (const auto& bi : node->body) {
                    if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                        pushFn(mfd->get());
                    } else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi)) {
                        // `private do ... end` / `public do ... end` — its
                        // methods belong to the same type (visibility is
                        // erased at the BEAM level).
                        if (*vb) for (const auto& vi : (*vb)->items)
                            if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                pushFn(vfd->get());
                    }
                    // TypeAnnotation items in a make block are erased.
                }
                // Splice in trait default methods this type doesn't override.
                for (const auto* fd : inheritedDefaults(*node)) pushFn(fd);
                flushM();
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (!node) return;
                if (node->synthetic) {
                    // Top-level `let name = expr` → a 0-arity function.
                    for (const auto& e : node->body) {
                        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
                            if (!le->pattern) continue;
                            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                                L.subst.clear();
                                FunDef def; def.name = vp->name; def.arity = 0;
                                FunClause fc; fc.body = L.lower(le->value);
                                def.clauses.push_back(std::move(fc));
                                mod.functions.push_back(std::move(def));
                                continue;
                            }
                            // `let (a, b) = …` at top level: one 0-arity
                            // function per bound name. Without these the names
                            // compiled to nothing and every later reference
                            // died with "Undefined variable".
                            std::vector<std::string> names;
                            patternBoundNames(le->pattern, names);
                            for (const auto& name : names) {
                                L.subst.clear();
                                FunDef def; def.name = name; def.arity = 0;
                                FunClause fc;
                                fc.body = L.topLevelPatternBinding(*le, name);
                                def.clauses.push_back(std::move(fc));
                                mod.functions.push_back(std::move(def));
                            }
                        }
                    }
                } else if (node->isExplicitMain) {
                    L.subst.clear();
                    for (const auto& p : node->params)
                        if (p.name) L.subst[*p.name] = *p.name;
                    FunDef def; def.name = "main";
                    ExprPtr body = L.lowerBody(node->body);
                    FunClause fc;
                    if (node->params.empty()) {
                        def.arity = 0; mod.mainArity = 0;
                    } else {
                        // main(args) / main(args, env): param 0 is the args
                        // list (from init:get_plain_arguments); a second param
                        // is the ENV map, bound in the body.
                        def.arity = 1; mod.mainArity = 1;
                        if (node->params.size() >= 2 && node->params[1].name)
                            body = L.makeLet(*node->params[1].name,
                                             L.callE("kex_io", "env_map", 0, {}), std::move(body));
                        auto p = std::make_unique<Pattern>();
                        p->kind = PatKind::Var;
                        p->name = node->params[0].name ? *node->params[0].name : "_args";
                        fc.params.push_back(std::move(p));
                    }
                    if (node->rescue) body = L.wrapWithTryCatch(std::move(body), *node->rescue);
                    fc.body = L.withTestSummary(L.withDisplayInfo(std::move(body)));
                    def.clauses.push_back(std::move(fc));
                    mod.functions.push_back(std::move(def));
                    mod.hasMain = true;
                } else {
                    // Bare top-level expression(s) → accumulate for a synthetic main.
                    for (const auto& e : node->body) bareExprs.push_back(&e);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                // Erased: signatures produce nothing; default methods are
                // spliced into each implementing type's method group above.
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>> ||
                                 std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                // Types/annotations are erased, but collect variant tags first
                // so dispatchers can wildcard-match them (see the nested module
                // handler below for the same logic).
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (node && node->variants) {
                        std::vector<std::string> tags;
                        for (const auto& v : *node->variants) {
                            auto t = Lowering::simpleTypeName(v);
                            if (!t.empty()) {
                                tags.push_back(t);
                                if (std::holds_alternative<ast::TypeName>(v->kind))
                                    L.nullaryVariantTags.insert(t);
                            }
                        }
                        if (!tags.empty() && tags.size() >= 2)
                            L.typeVariantTags[node->name] = std::move(tags);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) {
                    std::vector<const ast::FunctionDef*> grp;
                    auto flush = [&]{
                        if (!grp.empty()) {
                            auto it = L.moduleFunctions.find(L.currentModulePath + "." + grp.front()->name);
                            mod.functions.push_back(L.lowerFunctionGroup(grp, "",
                                it == L.moduleFunctions.end() ? grp.front()->name : it->second));
                            grp.clear();
                        }
                    };
                    auto push = [&](const ast::FunctionDef* fd) {
                        if (!fd) return;
                        if (!grp.empty() && (grp.front()->name != fd->name || beamArity(grp.front()) != beamArity(fd))) flush();
                        grp.push_back(fd);
                    };
                    auto emitMake = [&](const ast::MakeDef* mk) {
                        if (!mk) return;
                        std::string typeName = Lowering::simpleTypeName(mk->target);
                        std::vector<const ast::FunctionDef*> methods;
                        auto flushMethods = [&] {
                            if (!methods.empty()) {
                                mod.functions.push_back(L.lowerMakeGroup(methods, typeName));
                                methods.clear();
                            }
                        };
                        auto pushMethod = [&](const ast::FunctionDef* fd) {
                            if (!fd) return;
                            if (!methods.empty() && (methods.front()->name != fd->name ||
                                beamArity(methods.front()) != beamArity(fd))) flushMethods();
                            methods.push_back(fd);
                        };
                        for (const auto& mi : mk->body) {
                            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                                pushMethod(fd->get());
                            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                                if (*vb) for (const auto& vi : (*vb)->items)
                                    if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        pushMethod(fd->get());
                        }
                        for (const auto* fd : inheritedDefaults(*mk)) pushMethod(fd);
                        flushMethods();
                    };
                    std::function<void(const ast::ModuleDef&)> lowerModuleBody;
                    lowerModuleBody = [&](const ast::ModuleDef& m) {
                        auto savedModulePath = L.currentModulePath;
                        L.currentModulePath = m.name;
                        auto savedImports = L.moduleImports;
                        auto savedAliases = L.moduleAliases;
                        auto prefix = m.name + ".";
                        for (const auto& [key, val] : L.moduleFunctions)
                            if (key.rfind(prefix, 0) == 0) {
                                auto bare = key.substr(prefix.size());
                                if (bare.find('.') == std::string::npos)
                                    L.moduleImports[bare] = val;
                            }
                        auto emitType = [&](const ast::TypeDef* td) {
                            if (!td || !td->variants) return;
                            if (auto constructors =
                                    kex::typeConstructors(*td))
                                for (const auto& constructor :
                                     *constructors)
                                    mod.functions.push_back(
                                        moduleConstructorDef(
                                            constructor, m.name));
                            if (kex::isTransparentTypeAlias(*td))
                                return;
                            std::vector<std::string> tags;
                            for (const auto& variant :
                                 *td->variants) {
                                auto tag =
                                    Lowering::simpleTypeName(
                                        variant);
                                if (tag.empty()) continue;
                                tags.push_back(tag);
                                if (std::holds_alternative<
                                        ast::TypeName>(
                                        variant->kind))
                                    L.nullaryVariantTags.insert(
                                        tag);
                            }
                            if (!tags.empty())
                                L.typeVariantTags[td->name] =
                                    std::move(tags);
                        };
                        for (const auto& bi : m.body) {
                            if (auto* ub = std::get_if<std::unique_ptr<ast::UsingBlock>>(&bi)) {
                                if (!*ub) continue;
                                std::string srcMod;
                                for (size_t i = 0; i < (*ub)->module.parts.size(); i++) {
                                    if (i) srcMod += ".";
                                    srcMod += (*ub)->module.parts[i];
                                }
                                if ((*ub)->alias) L.moduleAliases[*(*ub)->alias] = srcMod;
                                auto importName = [&](const std::string& name) {
                                    auto key = srcMod + "." + name;
                                    if (auto it = L.moduleFunctions.find(key); it != L.moduleFunctions.end())
                                        L.moduleImports[name] = it->second;
                                };
                                if (!(*ub)->onlyNames.empty()) {
                                    for (const auto& name : (*ub)->onlyNames) importName(name);
                                } else if ((*ub)->body.empty()) {
                                    for (const auto& [key, val] : L.moduleFunctions)
                                        if (key.rfind(srcMod + ".", 0) == 0) {
                                            auto bare = key.substr(srcMod.size() + 1);
                                            if (bare.find('.') == std::string::npos
                                                && std::find((*ub)->exceptNames.begin(),
                                                             (*ub)->exceptNames.end(), bare)
                                                    == (*ub)->exceptNames.end())
                                                L.moduleImports[bare] = val;
                                        }
                                }
                                continue;
                            } else if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                                push(mfd->get());
                            } else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi)) {
                                if (*vb) for (const auto& vi : (*vb)->items) {
                                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        push(vfd->get());
                                    else if (auto* vtd =
                                                 std::get_if<
                                                     std::unique_ptr<
                                                         ast::TypeDef>>(
                                                     &vi)) {
                                        flush();
                                        emitType(vtd->get());
                                    }
                                }
                            } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&bi)) {
                                flush();
                                emitMake(mk->get());
                            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&bi)) {
                                if (*cb) for (const auto& ci : (*cb)->items) {
                                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci)) {
                                        push(cfd->get());
                                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                                        flush();
                                        emitMake(cmd->get());
                                    }
                                }
                            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&bi)) {
                                flush();
                                if (*child) lowerModuleBody(**child);
                            } else if (std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&bi) ||
                                       std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                    flush();
                                    emitType(td->get());
                                }
                            } else {
                                flush();
                            }
                        }
                        flush();
                        L.moduleImports = std::move(savedImports);
                        L.moduleAliases = std::move(savedAliases);
                        L.currentModulePath = std::move(savedModulePath);
                    };
                    lowerModuleBody(*node);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (!node) return;
                std::string srcMod;
                for (size_t i = 0; i < node->module.parts.size(); i++) {
                    if (i) srcMod += ".";
                    srcMod += node->module.parts[i];
                }
                if (node->alias) L.moduleAliases[*node->alias] = srcMod;
                if (!node->onlyNames.empty()) {
                    for (const auto& name : node->onlyNames) {
                        auto key = srcMod + "." + name;
                        if (auto it = L.moduleFunctions.find(key); it != L.moduleFunctions.end())
                            L.moduleImports[name] = it->second;
                    }
                } else {
                    for (const auto& [key, val] : L.moduleFunctions)
                        if (key.rfind(srcMod + ".", 0) == 0) {
                            auto bare = key.substr(srcMod.size() + 1);
                            if (bare.find('.') == std::string::npos
                                && std::find(node->exceptNames.begin(),
                                             node->exceptNames.end(), bare)
                                    == node->exceptNames.end())
                                L.moduleImports[bare] = val;
                        }
                }
            } else {
                throw LowerError(std::string("IR lower: unimplemented top-level item ")
                                 + typeid(T).name());
            }
        }, item);
    }
    flushGroup();

    // Synthetic main/0 from bare top-level expressions, if no explicit main.
    if (!bareExprs.empty() && !mod.hasMain) {
        L.subst.clear();
        FunDef def; def.name = "main"; def.arity = 0;
        FunClause fc;
        // Emit each expr as a statement; last is the value.
        std::function<ExprPtr(size_t)> chain = [&](size_t i) -> ExprPtr {
            if (i + 1 == bareExprs.size()) return L.lower(*bareExprs[i]);
            auto val = L.lower(*bareExprs[i]);
            auto rest = chain(i + 1);
            auto let = std::make_unique<Expr>();
            let->node = Let{L.fresh("S"), std::move(val), std::move(rest)};
            return let;
        };
        fc.body = L.withTestSummary(L.withDisplayInfo(
            bareExprs.empty() ? lit(LitKind::Atom, "ok") : chain(0)));
        def.clauses.push_back(std::move(fc));
        mod.functions.push_back(std::move(def));
        mod.hasMain = true; mod.mainArity = 0;
    }
    // Pure-declaration program (no main block, no bare exprs): synthesize an
    // empty main/0 so `kex -R file.kex` runs it as the no-op it is on the
    // walker, instead of erl dying with undef on the missing main.
    if (!mod.hasMain) {
        FunDef def; def.name = "main"; def.arity = 0;
        FunClause fc; fc.body = lit(LitKind::Atom, "ok");
        def.clauses.push_back(std::move(fc));
        mod.functions.push_back(std::move(def));
        mod.hasMain = true; mod.mainArity = 0;
    }

    // Operators do not have a MethodCall node from which lowering can consume
    // the semantic winner. Keep the readable exact implementations, and make
    // the bare operator a small receiver/RHS-type dispatcher.
    for (const auto& [key, overloads] : L.argumentOverloadSignatures) {
        auto firstBreak = key.find('\n');
        auto lastBreak = key.rfind('\n');
        if (firstBreak == std::string::npos || firstBreak == lastBreak)
            continue;
        auto name = key.substr(0, firstBreak);
        if (name.empty() ||
            std::isalnum(static_cast<unsigned char>(name.front())) ||
            name.front() == '_')
            continue;
        auto arity = std::stoi(key.substr(lastBreak + 1));
        mod.functions.push_back(
            L.makeArgumentDispatcher(name, arity, overloads));
    }

    // Cross-type collision dispatchers (bare name → runtime-type dispatch).
    // Generated PER ARITY, including only the owner types that actually have a
    // `name/Type` variant at that BEAM arity — a method can be overloaded by arity across
    // types (e.g. `count/1` on List vs `count/2` on List+Map), and one dispatcher
    // per arity avoids referencing a variant that doesn't exist.
    for (const auto& name : L.collidingMethods) {
        std::map<int, std::vector<std::string>> ownersByArity;
        std::string prefix = receiverImplementationPrefix(name);
        for (const auto& fn : mod.functions)
            if (fn.name.rfind(prefix, 0) == 0)
                ownersByArity[fn.arity].push_back(fn.name.substr(prefix.size()));
        for (const auto& [arity, owners] : ownersByArity) {
            if (arity < 1) continue;
            // If every owner is an ADT type (has known variant tags), the
            // clauses from the mangled functions have distinct top-level
            // patterns — merge them into one function and skip the guard-
            // based dispatcher entirely. Pattern matching handles dispatch
            // natively, avoiding Core Erlang guard short-circuit issues.
            // Merging clauses works when the owners' patterns are distinct
            // ADT variants — but not when the name is also a record FIELD.
            // A method with an implicit `this` is an unguarded catch-all, so
            // merged clauses swallow every receiver and the field would never
            // be read: `tagged.name` returned the METHOD's answer on BEAM
            // while the walker read the field. Those go through the guarded
            // dispatcher below, which appends the field clauses.
            bool allAdt = !owners.empty() && !L.fieldAccessors.count(name);
            for (const auto& o : owners) {
                if (!L.typeVariantTags.count(o)) { allAdt = false; break; }
            }
            if (allAdt) {
                FunDef merged; merged.name = name; merged.arity = arity;
                for (size_t i = 0; i < mod.functions.size(); ) {
                    auto& f = mod.functions[i];
                    std::string prefix = receiverImplementationPrefix(name);
                    if (f.name.rfind(prefix, 0) == 0 && f.arity == arity) {
                        for (auto& c : f.clauses)
                            merged.clauses.push_back(std::move(c));
                        // Remove the mangled function (avoid duplicate and keep
                        // mod.functions clean).
                        mod.functions.erase(mod.functions.begin() + i);
                    } else {
                        ++i;
                    }
                }
                mod.functions.push_back(std::move(merged));
            } else {
                mod.functions.push_back(L.makeDispatcher(name, arity, owners));
            }
        }
    }

    // Field accessors last (so definedFns is fully known).
    for (auto& acc : L.makeAccessors(definedFns)) mod.functions.push_back(std::move(acc));

    // Merge duplicate function definitions (same name + arity) by concatenating
    // their clauses. The prelude legitimately repeats a method across make blocks
    // for different types with different clause patterns (e.g. optional.kex
    // defines `or` for both Optional<X> and Result<X,E>); erlc needs a single
    // function with all the clauses unified, not two conflicting definitions.
    // Clauses arriving after an unguarded catch-all are unreachable (first
    // match wins) and are dropped — this happens when overlapping traits force
    // the same method body into two make blocks (e.g. Integer defines
    // identity/combine for both Monoid and Group).
    auto unguardedCatchAll = [](const FunClause& c) {
        return !c.guard &&
            std::all_of(c.params.begin(), c.params.end(),
                [](const PatternPtr& p){ return p->kind == PatKind::Var || p->kind == PatKind::Wild; });
    };
    {
        std::map<std::pair<std::string, int>, FunDef> merged;
        for (auto& f : mod.functions) {
            auto key = std::make_pair(f.name, f.arity);
            auto it = merged.find(key);
            if (it == merged.end()) {
                auto& fnc = f;
                // Truncate clauses shadowed by an earlier catch-all within the
                // same definition as well.
                for (size_t i = 0; i < fnc.clauses.size(); i++)
                    if (unguardedCatchAll(fnc.clauses[i]) && i + 1 < fnc.clauses.size())
                        fnc.clauses.resize(i + 1);
                merged.emplace(key, std::move(fnc));
            } else {
                auto& existing = it->second;
                // If the existing definition already ends with a catch-all
                // but the incoming definition has specific patterns (e.g.
                // ADT variant matches), insert the specific clauses BEFORE
                // the catch-all so they get a chance to match first.
                bool existingHasCatchAll = std::any_of(
                    existing.clauses.begin(), existing.clauses.end(), unguardedCatchAll);
                if (existingHasCatchAll) {
                    bool incomingHasSpecific = std::any_of(
                        f.clauses.begin(), f.clauses.end(),
                        [&](const FunClause& c) { return !unguardedCatchAll(c); });
                    if (incomingHasSpecific) {
                        auto catchAllIt = std::find_if(
                            existing.clauses.begin(), existing.clauses.end(), unguardedCatchAll);
                        for (auto& c : f.clauses) {
                            if (!unguardedCatchAll(c))
                                catchAllIt = existing.clauses.insert(catchAllIt, std::move(c)) + 1;
                        }
                    }
                    continue;
                }
                existing.clauses.insert(existing.clauses.end(),
                    std::make_move_iterator(f.clauses.begin()),
                    std::make_move_iterator(f.clauses.end()));
            }
        }
        mod.functions.clear();
        for (auto& [_, f] : merged) mod.functions.push_back(std::move(f));
    }
    mod.typeVariantTags = L.typeVariantTags;
    return mod;
}

auto lowerProgramTiered(
    const ast::Program& prog,
    const std::array<size_t, 5>& tierBounds,
    const std::string& fileStem,
    const std::string& sourcePath,
    const ExternalModules* externals,
    const std::vector<ExternalRecordLayout>* externalRecords,
    const std::unordered_map<const ast::MethodCall*,
        semantic::ResolvedCallTarget>* resolvedCalls,
    bool preferExternalReceivers) -> Module {
    // The prelude still lowers as one merged module, so the output is
    // lowerProgram's by construction and the two paths cannot drift apart.
    // The bounds only validate the tier partition for now; they become
    // load-bearing when tiers compile to separate units and cross-tier
    // receiver calls must route through imported interfaces.
    for (size_t t = 0; t < 4; t++)
        if (tierBounds[t] > tierBounds[t + 1])
            throw LowerError("IR lower: non-monotonic prelude tier bounds");
    if (tierBounds[4] != prog.items.size())
        throw LowerError("IR lower: tier bounds do not cover the full prelude AST");
    return lowerProgram(prog, fileStem, sourcePath, externals, externalRecords,
                        resolvedCalls, preferExternalReceivers);
}

namespace {

auto rewriteModuleCalls(ExprPtr& expr,
                        const std::unordered_map<std::string, std::pair<std::string, std::string>>& targets)
    -> void {
    if (!expr) return;
    std::visit([&](auto& node) {
        using T = std::decay_t<decltype(node)>;
        auto visit = [&](ExprPtr& child) { rewriteModuleCalls(child, targets); };
        if constexpr (std::is_same_v<T, Call>) {
            if (node.module.empty()) {
                if (auto it = targets.find(node.name); it != targets.end()) {
                    node.module = it->second.first;
                    node.name = it->second.second;
                }
            }
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, Intrinsic>) {
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            visit(node.callee); for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, Let>) {
            visit(node.value); visit(node.body);
        } else if constexpr (std::is_same_v<T, Seq>) {
            for (auto& item : node.exprs) visit(item);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (auto& subject : node.subjects) visit(subject);
            for (auto& clause : node.clauses) {
                if (clause.guard) visit(*clause.guard);
                visit(clause.body);
            }
        } else if constexpr (std::is_same_v<T, Construct>) {
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, MakeTuple>) {
            for (auto& item : node.elements) visit(item);
        } else if constexpr (std::is_same_v<T, MakeList>) {
            for (auto& item : node.elements) visit(item);
            if (node.rest) visit(*node.rest);
        } else if constexpr (std::is_same_v<T, FieldGet>) {
            visit(node.record);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            visit(node.body);
        } else if constexpr (std::is_same_v<T, Return>) {
            visit(node.value);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            visit(node.funBody); visit(node.contBody);
        } else if constexpr (std::is_same_v<T, TryThrow>) {
            visit(node.error);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            visit(node.body);
            for (auto& clause : node.clauses) {
                if (clause.guard) visit(*clause.guard);
                visit(clause.body);
            }
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (auto& clause : node.clauses) {
                visit(clause.body);
            }
            if (node.timeout) visit(*node.timeout);
            if (node.afterBody) visit(*node.afterBody);
        }
    }, expr->node);
}

} // namespace

auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const std::string& sourcePath,
                  const std::vector<ExternalRecordLayout>* externalRecords,
                  const ExternalModules* externals,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls,
                  bool preferExternalReceivers,
                  const std::vector<ExternalVariantTag>* externalVariants,
                  const StaticTypeOfCalls* staticTypeOfCalls)
    -> std::vector<Module> {
    auto flat = lowerProgram(prog, fileStem, sourcePath, externals,
                             externalRecords, resolvedCalls,
                             preferExternalReceivers, externalVariants,
                             staticTypeOfCalls);

    struct Definition {
        std::string path;
        std::string sourceName;
        bool exported;
        // External record accessors are already compiled in their owner
        // module (not necessarily `Kex.<path>`).
        std::string beamModule;
    };
    std::unordered_map<std::string, Definition> definitions;
    if (externalRecords) {
        // Every field name this program's own records declare. Routing one of
        // those to the owner module would hand `myRecord.value` to the
        // prelude's `value/1`, whose clauses only cover the records IT knows —
        // an `if_clause` crash on BEAM for a record it has never heard of.
        // The locally emitted accessor already dispatches on external layouts
        // too (collectRecordLayout feeds fieldAccessors), so keeping it local
        // serves both.
        std::unordered_set<std::string> localRecordFields;
        // ...and every name a `make` block of this program defines as a
        // METHOD. Same reason, one step further: `make Widget do let value()`
        // is the owner of `value` here, and routing the call to the prelude's
        // `Measure` accessor read element 2 of the widget instead — silently,
        // returning a plausible wrong number rather than crashing. The
        // interpreter always got this right, so it was a backend divergence
        // too.
        std::unordered_set<std::string> localMethodNames;
        std::function<void(const ast::RecordDef&)> noteRecord =
            [&](const ast::RecordDef& rd) {
                for (const auto& field : rd.fields) localRecordFields.insert(field.name);
            };
        std::function<void(const ast::MakeDef&)> noteMake =
            [&](const ast::MakeDef& mk) {
                for (const auto& mi : mk.body) {
                    if (auto* fd =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi)) {
                        if (*fd) localMethodNames.insert((*fd)->name);
                    } else if (auto* vb = std::get_if<
                                   std::unique_ptr<ast::VisibilityBlock>>(&mi)) {
                        if (*vb)
                            for (const auto& vi : (*vb)->items)
                                if (auto* vfd = std::get_if<
                                        std::unique_ptr<ast::FunctionDef>>(&vi))
                                    if (*vfd) localMethodNames.insert((*vfd)->name);
                    }
                }
            };
        std::function<void(const ast::ModuleDef&)> scanModule;
        auto scanItems = [&](const auto& items, auto&& self) -> void {
            for (const auto& item : items) {
                if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                    if (*rd) noteRecord(**rd);
                } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                    if (*mk) noteMake(**mk);
                } else if (auto* md = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                    if (*md) scanModule(**md);
                }
            }
            (void)self;
        };
        scanModule = [&](const ast::ModuleDef& module) {
            scanItems(module.body, scanItems);
        };
        scanItems(prog.items, scanItems);

        for (const auto& record : *externalRecords) {
            if (record.moduleAtom.empty()) continue;
            for (const auto& field : record.fields) {
                // A field accessor name can also be a real method (notably
                // Unit.kind/factor/symbol). Route only the unambiguous record
                // fields here; make-method dispatch remains module-local.
                if (field != "unit" && field != "canonical" && field != "value" &&
                    field != "conversionFactor" && field != "dimension" && field != "notation")
                    continue;
                if (localRecordFields.count(field)) continue;
                if (localMethodNames.count(field)) continue;
                definitions.emplace(field, Definition{"", field, true, record.moduleAtom});
            }
        }
    }
    std::vector<std::string> modulePaths;
    std::unordered_set<std::string> seenModulePaths;
    std::function<void(const ast::ModuleDef&)> collect;
    collect = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        if (seenModulePaths.insert(path).second)
            modulePaths.push_back(path);
        auto add = [&](const ast::FunctionDef* fn, bool exported) {
            if (fn)
                definitions[mangleModuleMember(path, fn->name)] =
                    {path, fn->name, exported};
        };
        auto addMake = [&](const ast::MakeDef* mk) {
            if (!mk) return;
            const std::string typeName = Lowering::simpleTypeName(mk->target);
            auto collectMethod = [&](const ast::FunctionDef* fd) {
                if (!fd) return;
                if (!definitions.count(fd->name))
                    definitions[fd->name] = {path, fd->name, true};
                // A make method is emitted as `method/Type` when the same
                // method name has implementations for multiple receiver
                // types. Keep that generated name in its declaring module;
                // otherwise module-local calls get mistaken for flat/global
                // functions and point at `kex_<stem>` on BEAM.
                if (!typeName.empty())
                    definitions[mangleReceiverImplementation(
                        fd->name, typeName)] =
                        {path, mangleReceiverImplementation(
                            fd->name, typeName), true};
                if (!typeName.empty()) {
                    auto exact = mangleReceiverSignature(
                        fd->name, methodDispatchTypes(*fd, typeName));
                    definitions[exact] = {path, exact, true};
                }
            };
            for (const auto& mi : mk->body) {
                if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                    collectMethod(fd->get());
                else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            collectMethod(fd->get());
            }
        };
        auto addRecord = [&](const ast::RecordDef* rd) {
            if (!rd) return;
            for (const auto& field : rd->fields)
                if (!definitions.count(field.name))
                    definitions[field.name] = {path, field.name, true};
        };
        auto addType = [&](const ast::TypeDef* td, bool exported) {
            if (!td) return;
            if (auto constructors = kex::typeConstructors(*td))
                for (const auto& constructor : *constructors)
                    definitions[mangleModuleMember(
                        path, constructor.name)] = {
                        path, constructor.name, exported};
        };
        for (const auto& item : module.body) {
            if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) add(fn->get(), true);
            else if (auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                if (*visibility) for (const auto& entry : (*visibility)->items) {
                    if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry))
                        add(fn->get(), (*visibility)->isPublic);
                    else if (auto* td =
                                 std::get_if<std::unique_ptr<
                                     ast::TypeDef>>(&entry))
                        addType(td->get(),
                                (*visibility)->isPublic);
                }
            } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                addMake(mk->get());
            } else if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                addRecord(rd->get());
            } else if (auto* td =
                           std::get_if<std::unique_ptr<ast::TypeDef>>(
                               &item)) {
                addType(td->get(), true);
            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*child) collect(**child);
            }
        }
    };
    for (const auto& item : prog.items)
        if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item); module && *module)
            collect(**module);

    std::unordered_map<std::string, std::pair<std::string, std::string>> targets;
    for (const auto& [emitted, def] : definitions)
        targets[emitted] = {def.beamModule.empty() ? "Kex." + def.path : def.beamModule,
                            def.sourceName};

    std::vector<Module> result;
    std::unordered_map<std::string, std::vector<FunDef>> moduleBuckets;
    std::vector<FunDef> globalFunctions;
    for (auto& fn : flat.functions) {
        auto found = definitions.find(fn.name);
        if (found == definitions.end()) {
            globalFunctions.push_back(std::move(fn));
            continue;
        }
        const auto& def = found->second;
        fn.name = def.sourceName;
        fn.exported = def.exported;
        moduleBuckets[def.path].push_back(std::move(fn));
    }
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        globalTargets;
    for (const auto& fn : globalFunctions)
        globalTargets[fn.name] = {flat.name, fn.name};
    flat.functions = std::move(globalFunctions);

    result.push_back(std::move(flat));
    for (const auto& path : modulePaths) {
        Module module;
        module.name = "Kex." + path;
        if (auto it = moduleBuckets.find(path); it != moduleBuckets.end())
            module.functions = std::move(it->second);
        result.push_back(std::move(module));
    }

    for (size_t moduleIndex = 0; moduleIndex < result.size(); moduleIndex++) {
        auto& module = result[moduleIndex];
        for (auto& fn : module.functions)
            for (auto& clause : fn.clauses) {
                if (clause.guard) rewriteModuleCalls(*clause.guard, targets);
                rewriteModuleCalls(clause.body, targets);
                if (moduleIndex > 0) {
                    if (clause.guard)
                        rewriteModuleCalls(*clause.guard, globalTargets);
                    rewriteModuleCalls(clause.body, globalTargets);
                }
            }
    }
    return result;
}

} // namespace kex::ir

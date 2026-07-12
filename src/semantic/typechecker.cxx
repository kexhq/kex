#include "typechecker.hxx"
#include "analyzer.hxx"
#include <functional>
#include <set>
#include <unordered_set>

namespace kex::semantic {

namespace {

// A sum-type variant is constructor-shaped (TypeName or GenericType with a
// single-part name) if it's a bare `Name` or `Name(Args...)` — anything
// else (tuple/list/function/union/atom type exprs) means the TypeDef is a
// type alias, not a real ADT, and the whole def is skipped for exhaustiveness.
auto extractConstructorName(const ast::TypeExprPtr& variant) -> std::optional<std::string> {
    if (!variant) return std::nullopt;
    if (auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
        if (tn->parts.size() == 1) return tn->parts[0];
    }
    if (auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
        if (gt->name.parts.size() == 1) return gt->name.parts[0];
    }
    return std::nullopt;
}

} // namespace

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_scopeStack.clear();
    pushScope();

    // Built-in prelude ADTs (see src/interpreter/stdlib/adt.cxx) — Just/
    // None and Ok/Error are bare constructors, not declared via a user
    // `type ... = ...`, so they're registered here rather than discovered
    // by walking TypeDefs below.
    m_adtVariants["Option"] = {"Just", "None"};
    m_adtOfConstructor["Just"] = "Option";
    m_adtOfConstructor["None"] = "Option";
    m_adtVariants["Result"] = {"Ok", "Error"};
    m_adtOfConstructor["Ok"] = "Result";
    m_adtOfConstructor["Error"] = "Result";
    m_adtVariants["Comparison"] = {"Less", "Equal", "Greater"};
    m_adtOfConstructor["Less"] = "Comparison";
    m_adtOfConstructor["Equal"] = "Comparison";
    m_adtOfConstructor["Greater"] = "Comparison";
    m_adtVariants["Either"] = {"Left", "Right"};
    m_adtOfConstructor["Left"] = "Either";
    m_adtOfConstructor["Right"] = "Either";

    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }

    registerTypeAliases(program);

    // Register built-in types
    // (done before registerDeclaredSignatures so annotation TypeExprs can resolve these names)
    // Int is a user-facing alias for Integer (arbitrary precision).
    // Fixed-width integers are available as Int32, Int64, etc.
    m_globals.set("Int", Type::integer());
    m_globals.set("Integer", Type::integer());
    m_globals.set("Char", Type::charT());
    m_globals.set("String", Type::string());
    m_globals.set("Bool", Type::boolean());
    m_globals.set("Byte", Type::byte());
    m_globals.set("Int8", Type::int8());
    m_globals.set("Int16", Type::int16());
    m_globals.set("Int32", Type::int32());
    m_globals.set("Int64", Type::int64());
    m_globals.set("UInt8", Type::uint8());
    m_globals.set("UInt16", Type::uint16());
    m_globals.set("UInt32", Type::uint32());
    m_globals.set("UInt64", Type::uint64());
    m_globals.set("Float32", Type::float32());
    m_globals.set("Float64", Type::float64());
    // Note: no "Float" entry — it's not a concrete Type, only a trait
    // name (TraitRegistry, phase 3), satisfied by Float32 and Float64.

    m_globals.set("ENV", Type::map(Type::string(), Type::string()));

    registerTraits(program);
    registerRecordFields(program);
    registerDeclaredSignatures(program);
    preRegisterFunctionSigs(program);

    // Topological ordering: check functions whose callees are all known
    // before checking their callers, so forward-reference calls use real
    // (post-body-check) result types rather than provisional TypeVars.
    // SCCs (mutual recursion) are detected via back-edges in the DFS and
    // kept in their original relative order — pre-registration handles them.
    {
        // Collect names of all user-defined top-level functions.
        std::unordered_set<std::string> userFns;
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) userFns.insert(n->name);
                }
            }, item);
        }

        // Collect direct call-dependencies for each FunctionDef.
        // Only user-defined function names are tracked (stdlib is always
        // checked before any user function, so it never needs ordering).
        std::function<void(const ast::Expr&, std::set<std::string>&)> collectCalls =
            [&](const ast::Expr& e, std::set<std::string>& out) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    if (userFns.count(n.name)) out.insert(n.name);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    if (userFns.count(n.method)) out.insert(n.method);
                    if (n.receiver) collectCalls(*n.receiver, out);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    if (n.left) collectCalls(*n.left, out);
                    if (n.right) collectCalls(*n.right, out);
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    if (n.operand) collectCalls(*n.operand, out);
                } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.thenBody) if (ex) collectCalls(*ex, out);
                    for (const auto& [c, b] : n.elifs) {
                        if (c) collectCalls(*c, out);
                        for (const auto& ex : b) if (ex) collectCalls(*ex, out);
                    }
                    if (n.elseBody)
                        for (const auto& ex : *n.elseBody) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                    if (n.subject) collectCalls(*n.subject, out);
                    for (const auto& cl : n.clauses) {
                        if (cl.guard && *cl.guard) collectCalls(**cl.guard, out);
                        if (cl.body) collectCalls(*cl.body, out);
                    }
                } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    if (n.thenExpr) collectCalls(*n.thenExpr, out);
                    if (n.elseExpr) collectCalls(*n.elseExpr, out);
                } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                    if (n.expr) collectCalls(*n.expr, out);
                    if (n.condition) collectCalls(*n.condition, out);
                } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                    if (n.inner) collectCalls(*n.inner, out);
                }
            }, e.kind);
        };

        std::unordered_map<std::string, std::set<std::string>> deps;
        // Keep a stable list of (name, item-index) for ordering.
        std::vector<std::pair<std::string, size_t>> fnOrder;
        for (size_t i = 0; i < program.items.size(); i++) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (!n) return;
                    std::set<std::string> d;
                    for (const auto& clause : n->clauses)
                        for (const auto& ex : clause.body) if (ex) collectCalls(*ex, d);
                    d.erase(n->name); // self-recursion: don't depend on self
                    deps[n->name] = std::move(d);
                    fnOrder.emplace_back(n->name, i);
                }
            }, program.items[i]);
        }

        // DFS post-order toposort (handles cycles via visited/onStack sets;
        // back-edge functions are left in original order — pre-registration
        // ensures they still type-check via shared TypeVar unification).
        std::unordered_set<std::string> visited, onStack;
        std::vector<std::string> sorted;
        std::function<void(const std::string&)> dfs = [&](const std::string& name) {
            if (visited.count(name)) return;
            if (onStack.count(name)) return; // back edge: cycle, skip
            onStack.insert(name);
            if (deps.count(name))
                for (const auto& dep : deps.at(name)) dfs(dep);
            onStack.erase(name);
            visited.insert(name);
            sorted.push_back(name);
        };
        for (const auto& [name, _] : fnOrder) dfs(name);

        // Build check order: sorted fn names first (deps before callers),
        // then any non-fn top-level items in their original positions.
        // We do two passes: first check all FunctionDefs in sorted order,
        // then check everything else (types, records, make blocks, main).
        std::unordered_set<std::string> fnChecked;
        for (const auto& name : sorted) {
            // Check ALL FunctionDefs with this name (handles overloaded functions
            // sharing a name — both overloads must be checked and appended to the
            // same overload set). Mark the name done only after the full sweep.
            for (const auto& item : program.items) {
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n && n->name == name) checkTopLevel(item);
                    }
                }, item);
            }
            fnChecked.insert(name);
        }
        // Non-function items and any unchecked functions (e.g. inside modules).
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n && !fnChecked.count(n->name)) checkTopLevel(item);
                } else {
                    checkTopLevel(item);
                }
            }, item);
        }
    }

    popScope();
}

auto TypeChecker::registerRecordFields(const ast::Program& program) -> void {
    std::unordered_map<std::string, TypePtr> noGenerics;
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                auto& fields = m_recordFields[node->name];
                for (const auto& f : node->fields) {
                    fields[f.name] = f.type ? resolveTypeExpr(*f.type, noGenerics)
                                            : Type::unknown();
                }
            }
        }, item);
    }
}

auto TypeChecker::registerAdt(const ast::TypeDef& def) -> void {
    if (!def.variants) return;

    std::vector<std::string> names;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) return;  // not constructor-shaped — a type alias, skip entirely
        names.push_back(*name);
    }
    if (names.empty()) return;

    m_adtVariants[def.name] = names;
    for (const auto& name : names) {
        m_adtOfConstructor[name] = def.name;
    }
}

auto TypeChecker::registerAdtsInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::typeDefToType(const ast::TypeDef& def) -> TypePtr {
    if (!def.variants || def.variants->empty()) return Type::unknown();
    // Build variant types, then fold into a union if more than one.
    std::unordered_map<std::string, TypePtr> noGenerics;
    std::vector<TypePtr> parts;
    for (const auto& v : *def.variants) {
        if (v) parts.push_back(resolveTypeExpr(*v, noGenerics));
    }
    if (parts.empty()) return Type::unknown();
    if (parts.size() == 1) return parts[0];
    return std::make_shared<Type>(Type{UnionType{std::move(parts)}});
}

auto TypeChecker::registerTraits(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                if (!node) return;
                TraitDef td;
                td.name = node->name;
                for (const auto& bodyItem : node->body) {
                    std::visit([&](const auto& bi) {
                        using BT = std::decay_t<decltype(bi)>;
                        if constexpr (std::is_same_v<BT, std::unique_ptr<ast::TypeAnnotation>>) {
                            if (!bi) return;
                            auto sig = annotationToSignature(*bi);
                            if (sig) {
                                sig->isFoul = bi->isFoul;
                                td.requiredMethods.push_back(std::move(*sig));
                            }
                        }
                        // FunctionDef items are default implementations — registered
                        // for completeness but not added to requiredMethods.
                    }, bodyItem);
                }
                m_traits.define(std::move(td));
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                // Only register as alias if no variant is constructor-shaped.
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return; // ADT, not an alias
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return;
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::annotationToSignature(const ast::TypeAnnotation& ann) -> std::optional<Signature> {
    if (!ann.type) return std::nullopt;
    // Unroll `A -> B -> C` (right-nested FunctionType) into params=[A,B], result=C.
    std::unordered_map<std::string, TypePtr> genericVars;
    std::vector<TypePtr> params;
    const ast::TypeExpr* cur = ann.type.get();
    while (cur) {
        if (auto* ft = std::get_if<ast::FunctionType>(&cur->kind)) {
            params.push_back(ft->param ? resolveTypeExpr(*ft->param, genericVars) : Type::unknown());
            cur = ft->result.get();
        } else {
            break;
        }
    }
    TypePtr result = cur ? resolveTypeExpr(*cur, genericVars) : Type::unknown();
    if (params.empty()) {
        // Non-function annotation (e.g. `x : Int`) — treat as a zero-param
        // constant whose type IS the annotated type.
        return Signature{ann.name, {}, result};
    }
    return Signature{ann.name, std::move(params), result};
}

auto TypeChecker::registerDeclaredSignatures(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
            if (!*ann) continue;
            auto sig = annotationToSignature(**ann);
            if (!sig) continue;
            // Declared annotation wins — stored first so checkFunctionDef
            // can find it and verify the body against the declared type.
            m_annotationDeclared.insert((*ann)->name);
            auto& sigs = m_userSignatures[(*ann)->name];
            // Insert declared sig at front, replacing any same-arity inferred
            // one that was somehow already there (shouldn't happen in pre-pass
            // ordering, but guard for safety).
            sigs.insert(sigs.begin(), std::move(*sig));
        }
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) preRegisterFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (!node) return;
                for (const auto& modItem : node->body) {
                    std::visit([this](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (mn) preRegisterFunctionDef(*mn);
                        }
                    }, modItem);
                }
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionDef(const ast::FunctionDef& def) -> void {
    // Skip make-block methods (they have an implicit `this` that mis-counts
    // arity in checkCall's UFCS desugaring, same exclusion as checkFunctionDef).
    if (m_inMakeBlock) return;
    // Skip annotation-declared functions — registerDeclaredSignatures already
    // populated m_userSignatures for these and checkFunctionDef will use the
    // annotation as ground truth.
    if (m_annotationDeclared.count(def.name)) return;
    // Skip if already pre-registered (e.g. multi-clause function — all clauses
    // share the same def.name and the first call handles them all).
    if (m_userSignatures.count(def.name)) return;

    std::vector<Signature> provisional;
    for (const auto& clause : def.clauses) {
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params) {
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        }
        provisional.push_back(Signature{def.name, std::move(paramTypes), freshTypeVar()});
    }
    m_userSignatures[def.name] = std::move(provisional);
}

auto TypeChecker::checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> void {
    bool hasUnguardedCatchAll = false;
    std::set<std::string> covered;
    std::string adtName;
    bool inconclusive = false;

    for (const auto& clause : node.clauses) {
        bool guarded = clause.guard && *clause.guard;
        for (const auto& pat : clause.patterns) {
            if (!pat) continue;
            if (std::holds_alternative<ast::WildcardPattern>(pat->kind) ||
                std::holds_alternative<ast::VarPattern>(pat->kind)) {
                if (!guarded) hasUnguardedCatchAll = true;
                continue;
            }
            std::string ctorName;
            if (auto* ctor = std::get_if<ast::ConstructorPattern>(&pat->kind)) {
                ctorName = ctor->name;
            } else if (auto* lit = std::get_if<ast::LiteralPattern>(&pat->kind);
                       lit && lit->literal.type == TokenType::None) {
                // `None` lexes as its own TokenType::None (lexer.cxx), so
                // it parses as a LiteralPattern, not ConstructorPattern{
                // "None"} — treat it as the Option constructor it is.
                ctorName = "None";
            } else {
                continue;  // literal/list/tuple/record/range patterns don't drive ADT exhaustiveness
            }

            auto it = m_adtOfConstructor.find(ctorName);
            if (it == m_adtOfConstructor.end()) {
                inconclusive = true;  // unregistered constructor — can't prove the closed set
                continue;
            }
            if (adtName.empty()) adtName = it->second;
            else if (adtName != it->second) inconclusive = true;  // patterns span more than one ADT
            if (!guarded) covered.insert(ctorName);
        }
    }

    if (hasUnguardedCatchAll || inconclusive || adtName.empty()) return;

    std::vector<std::string> missing;
    for (const auto& ctorName : m_adtVariants[adtName]) {
        if (!covered.count(ctorName)) missing.push_back(ctorName);
    }
    if (missing.empty()) return;

    std::string list;
    for (size_t i = 0; i < missing.size(); i++) {
        if (i) list += ", ";
        list += missing[i];
    }
    error(loc, "Non-exhaustive match on " + adtName + ": missing case(s) " + list);
}

auto TypeChecker::resolveTypeExpr(const ast::TypeExpr& typeExpr,
                                   std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr {
    return std::visit([this, &genericVars](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const std::string& last = node.parts.back();
            // `This` inside a make block refers to the implementing type.
            if (last == "This" && node.parts.size() == 1 && m_inMakeBlock && !m_currentMakeType.empty())
                return Type::named(m_currentMakeType);
            // `Never` — the bottom/uninhabited type for never-returning functions.
            // `Void` is an alias for the unit type `()` (Swift-style naming).
            if (last == "Never" && node.parts.size() == 1)
                return Type::voidType();
            if (last == "Void" && node.parts.size() == 1)
                return Type::unit();
            // `Any` — escape hatch: unifies with everything (unknown message type
            // for Process<Any>, etc.). Treated as UnknownType at the type level.
            if (last == "Any" && node.parts.size() == 1)
                return Type::unknown();
            // Single-letter identifiers are generic type variables (docs/
            // types.md Generics) — reuse the same TypeVar for repeated
            // occurrences of the same letter within this clause.
            if (last.size() == 1 && node.parts.size() == 1) {
                auto it = genericVars.find(last);
                if (it != genericVars.end()) return it->second;
                auto var = freshTypeVar();
                genericVars[last] = var;
                return var;
            }
            if (auto known = m_globals.get(last)) return known;
            // Trait-only names (Float, Number, Comparable, ...) have no
            // concrete Type — m_globals deliberately has no entry for them
            // (see check()'s comment) — so a param annotated `Float` means
            // "any T satisfying Float", same as an explicit constraint.
            if (m_traits.get(last)) return Type::constrained(last, last);
            // User type alias (e.g. `type Level = :debug | :info | ...`)
            auto aliasIt = m_typeAliases.find(last);
            if (aliasIt != m_typeAliases.end()) return aliasIt->second;
            return Type::named(last);  // unregistered record/ADT name — nameable, not yet structurally known
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::vector<TypePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
            }
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            TypePtr param = node.param ? resolveTypeExpr(*node.param, genericVars) : Type::unknown();
            TypePtr result = node.result ? resolveTypeExpr(*node.result, genericVars) : Type::unknown();
            return Type::func({param}, result);
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<TypePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? resolveTypeExpr(*elem, genericVars) : Type::unknown());
            }
            return Type::tuple(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveTypeExpr(*node.element, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(node.key ? resolveTypeExpr(*node.key, genericVars) : Type::unknown(),
                              node.value ? resolveTypeExpr(*node.value, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveTypeExpr(*node.inner, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<TypePtr> members;
            members.push_back(node.left ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
            members.push_back(node.right ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
            return std::make_shared<Type>(Type{UnionType{std::move(members)}});
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            auto it = genericVars.find(node.name);
            if (it != genericVars.end()) return it->second;
            auto var = freshTypeVar();
            genericVars[node.name] = var;
            return var;
        }
        else {
            // BlockType — not modeled as a distinct semantic::Type yet.
            return Type::unknown();
        }
    }, typeExpr.kind);
}

auto TypeChecker::bindPatternVars(const ast::Pattern& pat) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") defineVar(node.name, freshTypeVar());
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& arg : node.args) {
                if (arg) bindPatternVars(*arg);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            for (const auto& field : node.fields) {
                if (field.pattern) bindPatternVars(**field.pattern);
                else if (!field.isStringKey) defineVar(field.name, freshTypeVar());
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem);
            }
            if (node.rest) bindPatternVars(**node.rest);
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            // (x..y) in a match clause binds x and y as variables.
            if (node.start) bindPatternVars(*node.start);
            if (node.end) bindPatternVars(*node.end);
        }
        // LiteralPattern, WildcardPattern introduce nothing.
    }, pat.kind);
}

auto TypeChecker::checkTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            checkModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            checkFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            checkMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            checkMainBlock(*node);
        }
    }, item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    pushScope();
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                checkMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                checkModule(*node);
            }
        }, item);
    }
    popScope();
}

auto TypeChecker::checkFunctionDef(const ast::FunctionDef& def) -> void {
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    auto* declared = [&]() -> const Signature* {
        if (!m_annotationDeclared.count(def.name)) return nullptr;
        auto it = m_userSignatures.find(def.name);
        if (it != m_userSignatures.end() && !it->second.empty()) return &it->second[0];
        return nullptr;
    }();

    // Resolve inline return type annotation (-> Type on the function def line),
    // present when no separate TypeAnnotation declaration exists.
    TypePtr inlineReturnType;
    if (!declared && !def.clauses.empty() && def.clauses[0].returnAnnotation) {
        std::unordered_map<std::string, TypePtr> gvars;
        inlineReturnType = resolveTypeExpr(**def.clauses[0].returnAnnotation, gvars);
    }

    auto returnType = declared    ? declared->result
                    : inlineReturnType ? inlineReturnType
                    : freshTypeVar();
    defineVar(def.name, returnType);

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        pushScope();
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (declared && pi < declared->params.size() && !param.type) {
                paramType = declared->params[pi];
            } else {
                paramType = param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern);
            }
        }
        auto bodyType = inferBody(clause.body);
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        auto effectiveReturnType = declared ? declared->result : inlineReturnType;
        if (effectiveReturnType &&
            !std::holds_alternative<TypeVar>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, effectiveReturnType)) {
            error(def.location,
                  "`" + def.name + "` declared to return " + typeToString(effectiveReturnType) +
                  " but body returns " + typeToString(bodyType));
        }
        // Resolve TypeVars — body inference may have constrained unannotated
        // params via unifyVar (e.g. `n * 2` → n : Number).
        for (auto& pt : paramTypes) pt = resolve(pt);
        popScope();
        // Prefer the declared/annotated return type for annotated functions —
        // using the inferred body type would expose internal TypeVars to call
        // sites, which can be incorrectly constrained by the first call.
        auto concrete = [](const TypePtr& t) {
            return !std::holds_alternative<TypeVar>(t->kind);
        };
        auto resultType = (effectiveReturnType && concrete(effectiveReturnType))
                          ? effectiveReturnType : resolve(bodyType);
        signatures.push_back(Signature{def.name, std::move(paramTypes), resultType});
    }

    // make-block methods have an implicit `this` receiver, not a regular
    // param — checkCall's UFCS desugaring (receiver as argument 0) would
    // mis-count their arity, so they're checked (body inference still
    // runs above) but not registered for call-site checking.
    if (!m_inMakeBlock) {
        // If a declared signature already exists, update its result type with
        // the inferred one (keeping declared params) and keep one entry.
        if (declared) {
            auto& sigs = m_userSignatures[def.name];
            // Replace the placeholder declared sig with the fully-checked one.
            if (!signatures.empty()) sigs[0] = signatures[0];
        } else {
            if (m_checkedFunctions.count(def.name)) {
                // Additional `let f(...)` with the same name: append to the
                // overload set rather than replacing (typed overloads).
                for (auto& sig : signatures)
                    m_userSignatures[def.name].push_back(std::move(sig));
            } else {
                // First real check for this name: replace the provisional
                // pre-registered signatures with the inferred ones.
                m_userSignatures[def.name] = std::move(signatures);
                m_checkedFunctions.insert(def.name);
            }
        }
    }
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    pushScope();
    bool wasInMakeBlock = m_inMakeBlock;
    auto prevMakeType = m_currentMakeType;
    m_inMakeBlock = true;
    // Resolve the make-block's target type name for @field / `this` typing.
    if (def.target) {
        if (auto* tn = std::get_if<ast::TypeName>(&def.target->kind)) {
            if (tn->parts.size() == 1) m_currentMakeType = tn->parts[0];
        } else if (auto* gt = std::get_if<ast::GenericType>(&def.target->kind)) {
            // e.g. `make Map<K, V> do` or `make Option<A> do`
            if (gt->name.parts.size() == 1) m_currentMakeType = gt->name.parts[0];
        }
    }
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            }
        }, item);
    }
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = prevMakeType;
    popScope();

    checkTraitImplementation(def);
}

auto TypeChecker::checkTraitImplementation(const ast::MakeDef& def) -> void {
    if (def.implements.empty()) return;

    // Extract the type name from the make target.
    std::string typeName;
    if (def.target) {
        if (auto* tn = std::get_if<ast::TypeName>(&def.target->kind))
            if (tn->parts.size() == 1) typeName = tn->parts[0];
        if (auto* gt = std::get_if<ast::GenericType>(&def.target->kind))
            if (gt->name.parts.size() == 1) typeName = gt->name.parts[0];
    }

    // Collect method names and their foul status from this make block.
    std::unordered_map<std::string, bool> provided; // name -> isFoul
    for (const auto& item : def.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) provided[node->name] = node->isFoul;
            }
        }, item);
    }

    for (const auto& traitName : def.implements) {
        const TraitDef* trait = m_traits.get(traitName);
        if (!trait) {
            error(def.location, "Unknown trait: " + traitName);
            continue;
        }
        std::string prefix = "make " + (typeName.empty() ? "?" : typeName) +
                             ", implement: " + traitName + " — ";
        for (const auto& req : trait->requiredMethods) {
            auto it = provided.find(req.name);
            if (it == provided.end()) {
                error(def.location, prefix + "missing required method '" + req.name + "'");
            } else if (req.isFoul && !it->second) {
                error(def.location, prefix + "method '" + req.name +
                      "' must be declared foul (trait requires it)");
            }
        }
        // Register so satisfies() / argMatchesParam can verify trait usage.
        if (!typeName.empty()) {
            m_traits.registerImplementation(typeName, traitName);
        }
    }
}

auto TypeChecker::checkMainBlock(const ast::MainBlock& block) -> void {
    if (!block.synthetic) pushScope();
    for (size_t i = 0; i < block.params.size(); i++) {
        const auto& param = block.params[i];
        if (param.name.has_value() && *param.name != "_") {
            TypePtr type = (i == 0) ? Type::list(Type::string())
                         : (i == 1) ? Type::map(Type::string(), Type::string())
                                    : freshTypeVar();
            defineVar(*param.name, std::move(type));
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern);
        }
    }
    inferBody(block.body);
    if (!block.synthetic) popScope();
}

auto TypeChecker::inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr {
    TypePtr lastType = Type::unit();
    for (const auto& expr : body) {
        if (expr) lastType = inferExpr(*expr);
    }
    return lastType;
}

auto TypeChecker::resolveBlockHints(const std::string& name,
                                     const std::vector<TypePtr>& nonBlockArgTypes) -> std::vector<TypePtr> {
    const std::vector<Signature>* sigs = m_stdlib.lookup(name);
    if (!sigs) {
        auto it = m_userSignatures.find(name);
        if (it != m_userSignatures.end()) sigs = &it->second;
    }
    if (!sigs) return {};

    for (const auto& sig : *sigs) {
        if (sig.params.size() != nonBlockArgTypes.size() + 1) continue;
        auto* blockParam = std::get_if<FuncType>(&sig.params.back()->kind);
        if (!blockParam) continue;

        bool allMatch = true;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            if (!argMatchesParam(nonBlockArgTypes[i], sig.params[i])) { allMatch = false; break; }
        }
        if (!allMatch) continue;

        // Map negative-ID generic placeholders to concrete types from the actual args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(nonBlockArgTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : blockParam->params) hints.push_back(applySubst(p));
        return hints;
    }
    return {};
}

auto TypeChecker::resolveArgHints(const std::string& name,
                                   const std::vector<TypePtr>& argTypes,
                                   size_t slArgIdx) -> std::vector<TypePtr> {
    const std::vector<Signature>* sigs = m_stdlib.lookup(name);
    if (!sigs) {
        auto it = m_userSignatures.find(name);
        if (it != m_userSignatures.end()) sigs = &it->second;
    }
    if (!sigs) return {};

    for (const auto& sig : *sigs) {
        if (sig.params.size() != argTypes.size()) continue;
        if (slArgIdx >= sig.params.size()) continue;
        auto* funcParam = std::get_if<FuncType>(&sig.params[slArgIdx]->kind);
        if (!funcParam) continue;

        // All non-SL positions must match.
        bool allMatch = true;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            if (!argMatchesParam(argTypes[i], sig.params[i])) { allMatch = false; break; }
        }
        if (!allMatch) continue;

        // Build generic substitution from the concrete args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : funcParam->params) hints.push_back(applySubst(p));
        return hints;
    }
    return {};
}

auto TypeChecker::inferBlock(const ast::Expr& blockExpr,
                             const std::vector<TypePtr>& hintParams) -> TypePtr {
    if (auto* lam = std::get_if<ast::Lambda>(&blockExpr.kind)) {
        if (lam->params.empty()) {
            // Zero-param lambda `{ }` / `do end` — infer body but stay permissive
            // so it matches any FuncType param (the block ignores the passed arg).
            inferExpr(blockExpr);
            return Type::unknown();
        }
        pushScope();
        std::vector<TypePtr> paramTypes;
        for (size_t i = 0; i < lam->params.size(); i++) {
            TypePtr pt;
            if (i < hintParams.size()) {
                auto hint = resolve(hintParams[i]);
                if (!std::holds_alternative<TypeVar>(hint->kind) &&
                    !std::holds_alternative<UnknownType>(hint->kind)) {
                    pt = hint;
                } else {
                    pt = freshTypeVar();
                }
            } else {
                pt = freshTypeVar();
            }
            paramTypes.push_back(pt);
            if (lam->params[i].name != "_") defineVar(lam->params[i].name, pt);
        }
        auto bodyType = inferBody(lam->body);
        popScope();
        return Type::func(std::move(paramTypes), resolve(bodyType));
    }
    if (auto* sl = std::get_if<ast::ShorthandLambda>(&blockExpr.kind)) {
        if (sl->kind == ast::ShorthandLambda::Kind::Function && hintParams.size() > 1) {
            // Multi-param function capture: &+ used as (acc, elem) -> acc + elem.
            // Build a param list from all hints, call the function with all of them.
            std::vector<TypePtr> paramTypes;
            for (const auto& h : hintParams) {
                auto pt = resolve(h);
                paramTypes.push_back(
                    (std::holds_alternative<TypeVar>(pt->kind) ||
                     std::holds_alternative<UnknownType>(pt->kind))
                        ? freshTypeVar() : pt);
            }
            auto resultType = checkCall(sl->name, paramTypes, blockExpr.location);
            return Type::func(std::move(paramTypes), resolve(resultType));
        }
        TypePtr paramType = (!hintParams.empty()) ? resolve(hintParams[0]) : freshTypeVar();
        if (std::holds_alternative<TypeVar>(paramType->kind) ||
            std::holds_alternative<UnknownType>(paramType->kind))
            paramType = freshTypeVar();
        TypePtr resultType;
        if (sl->kind == ast::ShorthandLambda::Kind::Function) {
            resultType = checkCall(sl->name, {paramType}, blockExpr.location);
        } else {
            // &.method or &.method(args): UFCS → checkCall(name, [receiver, ...args])
            std::vector<TypePtr> callArgs = {paramType};
            for (const auto& arg : sl->args) {
                if (arg) callArgs.push_back(inferExpr(*arg));
            }
            resultType = checkCall(sl->name, callArgs, blockExpr.location);
        }
        return Type::func({paramType}, resolve(resultType));
    }
    // BlockExpr (no param list) — infer body for side effects, stay permissive.
    inferExpr(blockExpr);
    return Type::unknown();
}

auto TypeChecker::typeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_typeMap.find(expr);
    return it != m_typeMap.end() ? it->second : nullptr;
}

auto TypeChecker::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_typeMap;
}

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    auto result = std::visit([this, &expr](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Type::integer();
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Type::float64();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            return Type::string();
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Type::boolean();
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Type::charT();
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return Type::named("None");
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto type = lookupVar(node.name);
            if (!type) {
                return Type::unknown();
            }
            return type;
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                defineVar(varPat->name, valueType);
            } else if (node.pattern) {
                // Reject constructor mismatches early, e.g.
                //   let Ok(v) = parsePrefix(...)   when parsePrefix returns Optional
                //   let Just(v) = parse(...)       when parse returns Result
                if (auto* cp = std::get_if<ast::ConstructorPattern>(&node.pattern->kind)) {
                    auto resolved = resolve(valueType);
                    if (std::holds_alternative<UnknownType>(resolved->kind) ||
                        std::holds_alternative<TypeVar>(resolved->kind))
                        ; // permissive — can't determine the type at compile time
                    else if (cp->name == "Just" && !std::holds_alternative<OptionalType>(resolved->kind))
                        error(node.pattern->location, "cannot match `Just` — expected Optional, got " + typeToString(resolved));
                    else if (cp->name == "Ok" || cp->name == "Error") {
                        if (auto* nt = std::get_if<NamedType>(&resolved->kind)) {
                            if (nt->name != "Result")
                                error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        } else {
                            error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        }
                    }
                }
                bindPatternVars(*node.pattern);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            defineVar(node.name, valueType);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto varType = lookupVar(node.name);
            if (varType && !std::holds_alternative<UnknownType>(varType->kind) &&
                !std::holds_alternative<TypeVar>(varType->kind)) {
                if (!argMatchesParam(valueType, varType) &&
                    !std::holds_alternative<UnknownType>(valueType->kind) &&
                    !std::holds_alternative<TypeVar>(valueType->kind)) {
                    typeMismatch(expr.location, varType, valueType);
                }
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            auto leftType = node.left ? inferExpr(*node.left) : Type::unknown();
            auto rightType = node.right ? inferExpr(*node.right) : Type::unknown();
            return inferBinaryOp(node.op, leftType, rightType, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
            auto resolved = resolve(operandType);
            if (node.op == TokenType::Bang) {
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "Logical not '!' requires Bool, got " +
                          typeToString(resolved));
                }
                return Type::boolean();
            }
            if (node.op == TokenType::Minus) {
                // Unary negation requires a numeric operand.
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::constrained("N", "Number"));
                }
                return resolved;
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<TypePtr> argTypes;
            // First pass: infer concrete args; record ShorthandLambda positions.
            std::vector<size_t> slPositions;
            for (size_t i = 0; i < node.args.size(); i++) {
                if (node.args[i] &&
                    std::holds_alternative<ast::ShorthandLambda>(node.args[i]->kind)) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    slPositions.push_back(i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            for (const auto& [_, arg] : node.namedArgs) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            // Second pass: infer ShorthandLambda args with type hints from sig.
            for (size_t rawIdx : slPositions) {
                auto hints = resolveArgHints(node.name, argTypes, rawIdx);
                argTypes[rawIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(node.name, argTypes);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // send(pid, msg) — check msg type against Process<Msg> if pid type is known.
            if (node.name == "send" && argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        // Process<Any> (unknown msg type) skips the check.
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            return checkCall(node.name, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Namespace call: `Integer.parse(s)` → look up "Integer::parse".
            // The receiver is a bare UpperIdent — don't include it as argTypes[0].
            std::string callName = node.method;
            bool isNamespaceCall = node.receiver &&
                std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind);
            if (isNamespaceCall) {
                callName = std::get<ast::UpperIdentifier>(node.receiver->kind).name +
                           "::" + node.method;
            }
            std::vector<TypePtr> argTypes;
            if (!isNamespaceCall)
                argTypes.push_back(node.receiver ? inferExpr(*node.receiver) : Type::unknown());
            // First pass: infer concrete args; record ShorthandLambda positions.
            std::vector<size_t> slPositions;
            for (size_t i = 0; i < node.args.size(); i++) {
                if (node.args[i] &&
                    std::holds_alternative<ast::ShorthandLambda>(node.args[i]->kind)) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    slPositions.push_back(isNamespaceCall ? i : 1 + i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            for (const auto& [_, arg] : node.namedArgs) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            // Second pass: infer ShorthandLambda args with type hints from sig.
            for (size_t argIdx : slPositions) {
                auto hints = resolveArgHints(callName, argTypes, argIdx);
                argTypes[argIdx] = inferBlock(*node.args[argIdx - 1], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(callName, argTypes);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // pid.send(msg) UFCS — check msg type against Process<Msg>.
            // argTypes[0] = pid type, argTypes[1] = msg type.
            if (node.method == "send" && argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            return checkCall(callName, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    bool elemPermissive = std::holds_alternative<UnknownType>(elemType->kind) ||
                                         std::holds_alternative<TypeVar>(elemType->kind);
                    bool tPermissive = std::holds_alternative<UnknownType>(t->kind) ||
                                       std::holds_alternative<TypeVar>(t->kind) ||
                                       std::holds_alternative<FuncType>(t->kind);
                    bool elemFuncPermissive = elemPermissive ||
                                              std::holds_alternative<FuncType>(elemType->kind);
                    if (elemPermissive) {
                        elemType = t; // adopt the concrete type if we have one
                    } else if (!tPermissive && !elemFuncPermissive &&
                               !argMatchesParam(t, elemType) && !argMatchesParam(elemType, t)) {
                        // Before erroring, check if both types share a common trait.
                        // If so, widen the element type to that trait.
                        std::string common = m_traits.commonTrait(elemType, t);
                        if (!common.empty()) {
                            elemType = Type::named(common);
                        } else {
                            error(expr.location,
                                  "List elements must be the same type. Expected " +
                                  typeToString(elemType) + ", got " + typeToString(t));
                        }
                    }
                }
            }
            return Type::list(elemType);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            TypePtr keyType = Type::unknown();
            TypePtr valueType = Type::unknown();
            for (const auto& entry : node.entries) {
                if (entry.key) {
                    auto k = inferExpr(*entry.key);
                    if (std::holds_alternative<UnknownType>(keyType->kind)) keyType = k;
                }
                if (entry.value) {
                    auto v = inferExpr(*entry.value);
                    if (std::holds_alternative<UnknownType>(valueType->kind)) valueType = v;
                }
            }
            return Type::map(keyType, valueType);
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<TypePtr> types;
            for (const auto& elem : node.elements) {
                types.push_back(elem ? inferExpr(*elem) : Type::unknown());
            }
            return Type::tuple(std::move(types));
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            auto startType = node.start ? inferExpr(*node.start) : Type::unknown();
            if (node.end) inferExpr(*node.end);
            // Infer element type from the start bound: Char ranges → Range<Char>,
            // everything else → Range<Integer> (the common case).
            auto* prim = std::get_if<PrimitiveType>(&startType->kind);
            auto elemType = (prim && prim->kind == PrimitiveType::Char)
                ? Type::charT() : Type::integer();
            return Type::named("Range", {elemType});
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.letPattern) {
                // `if let Pattern = expr` — infer scrutinee, bind pattern vars
                // in a scope covering only the then-body (already scoped in
                // resolve pass). Skip Bool check — it's a pattern match.
                if (node.condition) inferExpr(*node.condition);
                pushScope();
                if (node.letPattern) bindPatternVars(*node.letPattern);
            } else if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            auto thenType = inferBody(node.thenBody);
            if (node.letPattern) popScope();
            TypePtr branchType = resolve(thenType);  // tracks the first concrete non-Never branch
            for (const auto& [cond, body] : node.elifs) {
                if (cond) inferExpr(*cond);
                auto elifType = resolve(inferBody(body));
                auto rt = resolve(branchType);
                bool rtPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                    std::holds_alternative<UnknownType>(rt->kind) ||
                                    std::holds_alternative<VoidType>(rt->kind);
                bool rePermissive = std::holds_alternative<TypeVar>(elifType->kind) ||
                                    std::holds_alternative<UnknownType>(elifType->kind) ||
                                    std::holds_alternative<VoidType>(elifType->kind);
                if (!rtPermissive && !rePermissive &&
                    !argMatchesParam(elifType, rt) && !argMatchesParam(rt, elifType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'elif' returns " + typeToString(elifType));
                }
                if (rtPermissive && !rePermissive) branchType = elifType;
            }
            if (node.elseBody) {
                auto elseType = resolve(inferBody(*node.elseBody));
                auto rt = resolve(branchType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind) ||
                                      std::holds_alternative<VoidType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(elseType->kind) ||
                                      std::holds_alternative<UnknownType>(elseType->kind) ||
                                      std::holds_alternative<VoidType>(elseType->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(elseType, rt) && !argMatchesParam(rt, elseType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(elseType));
                }
                if (thenPermissive && !elsePermissive) branchType = elseType;
            }
            return resolve(branchType);
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            TypePtr subjectType = node.subject ? inferExpr(*node.subject) : Type::unknown();
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.subjectBinding) {
                    defineVar(*node.subjectBinding, subjectType);
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) bindPatternVars(*pat);
                }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body) {
                    auto t = resolve(inferExpr(*clause.body));
                    auto rt = resolve(resultType);
                    if (std::holds_alternative<UnknownType>(rt->kind) ||
                        std::holds_alternative<TypeVar>(rt->kind)) {
                        resultType = t;  // adopt first concrete arm type
                    } else {
                        // Check subsequent arms match the first concrete arm.
                        bool armPermissive = std::holds_alternative<TypeVar>(t->kind) ||
                                            std::holds_alternative<UnknownType>(t->kind);
                        if (!armPermissive && !argMatchesParam(t, rt) && !argMatchesParam(rt, t)) {
                            error(expr.location, "Match arm type mismatch: expected " +
                                  typeToString(rt) + " but arm returns " + typeToString(t));
                        }
                    }
                }
                popScope();
            }
            checkMatchExhaustiveness(node, expr.location);
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return node.value ? inferExpr(*node.value) : Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            // Lambda used as a value (not a trailing block — that path goes
            // through inferBlock). Infer param types and body, return a proper
            // FuncType so call-site checking can validate the argument types.
            if (node.params.empty()) {
                auto bodyType = inferBody(node.body);
                return Type::func({}, resolve(bodyType));
            }
            pushScope();
            std::vector<TypePtr> paramTypes;
            for (const auto& param : node.params) {
                auto pt = freshTypeVar();
                paramTypes.push_back(pt);
                if (param.name != "_") defineVar(param.name, pt);
            }
            auto bodyType = inferBody(node.body);
            popScope();
            // Resolve param types after body inference — body may have constrained them.
            for (auto& pt : paramTypes) pt = resolve(pt);
            return Type::func(std::move(paramTypes), resolve(bodyType));
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            pushScope();
            inferBody(node.body);
            popScope();
            // Process<Msg> — Msg is a fresh TypeVar that unification resolves
            // against the declared return-type annotation (e.g. -> Process<Counter>).
            return Type::named("Process", {freshTypeVar()});
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            inferBody(node.body);
            return Type::voidType();  // infinite loop never returns
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "While condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            for (const auto& [_, val] : node.fields) {
                if (val) inferExpr(*val);
            }
            return Type::named(node.typeName);
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            if (node.expr) return inferExpr(*node.expr);
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return inferBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "then/else condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            auto thenType = node.thenExpr ? inferExpr(*node.thenExpr) : Type::unknown();
            if (node.elseExpr) {
                auto elseType = inferExpr(*node.elseExpr);
                auto rt = resolve(thenType);
                auto re = resolve(elseType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(re->kind) ||
                                      std::holds_alternative<UnknownType>(re->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(re, rt) && !argMatchesParam(rt, re)) {
                    error(expr.location, "Branch type mismatch: 'then' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(re));
                }
            }
            return resolve(thenType);
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            // &.method → (T) -> result; T unknown until used in context.
            auto paramType = freshTypeVar();
            TypePtr resultType;
            if (node.kind == ast::ShorthandLambda::Kind::Function) {
                resultType = checkCall(node.name, {paramType}, expr.location);
            } else {
                std::vector<TypePtr> callArgs = {paramType};
                for (const auto& arg : node.args) {
                    if (arg) callArgs.push_back(inferExpr(*arg));
                }
                resultType = checkCall(node.name, callArgs, expr.location);
            }
            return Type::func({paramType}, resolve(resultType));
        }
        else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            // Infer bound args for side-effect tracking.
            int boundCount = 0, openCount = 0;
            for (const auto& group : node.argGroups)
                for (const auto& arg : group)
                    if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                        openCount++;
                    else { inferExpr(*arg); boundCount++; }

            // Determine arity to compute remaining open param count.
            int arity = -1;
            if (node.isOperator) {
                arity = 2;
            } else {
                auto usit = m_userSignatures.find(node.name);
                if (usit != m_userSignatures.end() && !usit->second.empty())
                    arity = static_cast<int>(usit->second[0].params.size());
                else if (auto* sigs = m_stdlib.lookup(node.name); sigs && !sigs->empty())
                    arity = static_cast<int>((*sigs)[0].params.size());
            }

            // Remaining params = explicit placeholders + unfilled arity slots.
            int remaining = openCount;
            if (arity >= 0) remaining = std::max(openCount, arity - boundCount);

            // Build Func type with `remaining` fresh TypeVar params.
            if (remaining <= 0) return freshTypeVar(); // fully applied
            std::vector<TypePtr> params;
            for (int i = 0; i < remaining; i++) params.push_back(freshTypeVar());
            return Type::func(std::move(params), freshTypeVar());
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            // Inside a make block, `this` / `@field` has the record type.
            if (!m_currentMakeType.empty()) return Type::named(m_currentMakeType);
            return Type::unknown();
        }
        else {
            return Type::unknown();
        }
    }, expr.kind);
    m_typeMap[&expr] = result;
    return result;
}

auto TypeChecker::inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                                SourceLocation loc) -> TypePtr {
    // Type predicates used both in the TypeVar bail-out and the concrete section.
    auto isString = [](const TypePtr& t) {
        auto* list = std::get_if<ListType>(&t->kind);
        if (!list) return false;
        auto* elemPrim = std::get_if<PrimitiveType>(&list->element->kind);
        return elemPrim && elemPrim->kind == PrimitiveType::Char;
    };
    auto isChar = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Char;
    };
    auto isNumeric = [](const TypePtr& t) -> bool {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind))
            return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind) ||
               std::holds_alternative<SizedFloatType>(t->kind);
    };

    // Resolve TypeVars through the substitution map — a prior operation in the
    // same body may have already constrained them (e.g. `n - 1` constrains n
    // to Number before `n == 0` is checked).
    auto lhs = resolve(left);
    auto rhs = resolve(right);

    {
        auto lhsIsVar = std::holds_alternative<TypeVar>(lhs->kind);
        auto rhsIsVar = std::holds_alternative<TypeVar>(rhs->kind);
        auto lhsIsUnk = std::holds_alternative<UnknownType>(lhs->kind);
        auto rhsIsUnk = std::holds_alternative<UnknownType>(rhs->kind);

        if (lhsIsVar || rhsIsVar || lhsIsUnk || rhsIsUnk) {
            auto varId = [](const TypePtr& t) -> int {
                if (auto* tv = std::get_if<TypeVar>(&t->kind)) return tv->id;
                return -1;
            };
            auto isConcrete = [](const TypePtr& t) {
                return !std::holds_alternative<TypeVar>(t->kind) &&
                       !std::holds_alternative<UnknownType>(t->kind);
            };

            switch (op) {
                // -, *, /, %: both operands must be Number.
                case TokenType::Minus: case TokenType::Star:
                case TokenType::Slash: case TokenType::Percent: {
                    auto nc = Type::constrained("N", "Number");
                    if (int id = varId(lhs); id >= 0) unifyVar(id, nc);
                    if (int id = varId(rhs); id >= 0) unifyVar(id, nc);
                    if (isConcrete(lhs)) return lhs;
                    if (isConcrete(rhs)) return rhs;
                    return resolve(lhs);
                }
                // +: String/Char on one side → constrain TypeVar to String;
                //    numeric on one side → constrain TypeVar to Number.
                case TokenType::Plus: {
                    if (isConcrete(lhs) && (isString(lhs) || isChar(lhs))) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(rhs) && (isString(rhs) || isChar(rhs))) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(lhs) && isNumeric(lhs)) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return lhs;
                    }
                    if (isConcrete(rhs) && isNumeric(rhs)) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return rhs;
                    }
                    return lhs;
                }
                // Ordered comparisons: constrain TypeVar to the concrete side.
                case TokenType::LessThan: case TokenType::GreaterThan:
                case TokenType::LessEq: case TokenType::GreaterEq: {
                    if (int id = varId(lhs); id >= 0 && isConcrete(rhs)) unifyVar(id, rhs);
                    if (int id = varId(rhs); id >= 0 && isConcrete(lhs)) unifyVar(id, lhs);
                    return Type::boolean();
                }
                case TokenType::EqEq: case TokenType::NotEq:
                    return Type::boolean();
                case TokenType::AmpAmp: case TokenType::PipePipe: {
                    if (int id = varId(lhs); id >= 0) unifyVar(id, Type::boolean());
                    if (int id = varId(rhs); id >= 0) unifyVar(id, Type::boolean());
                    return Type::boolean();
                }
                case TokenType::QuestionQuestion:
                    // ?? returns rhs type when lhs is none, otherwise lhs type.
                    return rhs;
                default:
                    return lhs;
            }
        }
    }

    auto isFloat = [](const TypePtr& t) { return std::holds_alternative<SizedFloatType>(t->kind); };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(lhs), rightIsString = isString(rhs);
    bool leftIsFloat = isFloat(lhs), rightIsFloat = isFloat(rhs);
    bool leftIsInt = isIntegerLike(lhs), rightIsInt = isIntegerLike(rhs);

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString && rightIsString)
                return Type::string();
            // String + Char, Char + String, Char + Char — all produce String
            // (String = [Char], so appending a Char or two Chars is valid concatenation).
            if ((leftIsString || isChar(lhs)) && (rightIsString || isChar(rhs)))
                return Type::string();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot add " + typeToString(lhs) + " and " + typeToString(rhs));
                return Type::string();
            }
            if (!typesEqual(lhs, rhs)) {
                error(loc, "Operator '+' requires matching types, got " +
                      typeToString(lhs) + " and " + typeToString(rhs));
            }
            return lhs;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot use arithmetic operator on String");
                return Type::integer();
            }
            return lhs;

        case TokenType::EqEq:
        case TokenType::NotEq:
            return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(lhs)) {
                error(loc, "Cannot compare Bool values with '<', '>', '<=', '>='");
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(lhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(lhs));
            }
            if (!isBool(rhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(rhs));
            }
            return Type::boolean();

        case TokenType::QuestionQuestion:
            return rhs;

        default:
            return Type::unknown();
    }
}

auto TypeChecker::argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool {
    auto isPermissive = [](const TypePtr& t) {
        return std::holds_alternative<UnknownType>(t->kind) || std::holds_alternative<TypeVar>(t->kind);
    };
    if (isPermissive(argType) || isPermissive(paramType)) return true;
    // Never is the bottom type — a Never-typed expression (never returns) is
    // compatible with any expected type.
    if (std::holds_alternative<VoidType>(argType->kind)) return true;
    if (auto* constrained = std::get_if<ConstrainedType>(&paramType->kind)) {
        return m_traits.satisfies(argType, constrained->traitName);
    }
    // NamedType param that is itself a trait name: `Shape`, `Comparable`, etc.
    // Occurs when a heterogeneous list was widened to a trait element type and
    // then each element is checked against it, or when a trait-typed value is
    // passed to a ConstrainedType param that got resolved to NamedType.
    // argType matches if it implements that trait, or if argType IS that trait.
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        if (m_traits.get(paramNamed->name)) {
            if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
                argNamed && argNamed->name == paramNamed->name)
                return true; // trait-typed value matches trait param
            return m_traits.satisfies(argType, paramNamed->name);
        }
    }
    // Sized ints/floats and arbitrary-precision Integer aren't distinguished
    // at runtime yet (IntValue is one int64_t, FloatValue one double — see
    // the type-system plan's Runtime representation section), so don't
    // hard-error on a width/precision distinction the runtime doesn't keep:
    // any two Integer-trait members are compatible with each other, and
    // likewise for Float. Integer-vs-Float itself stays a real mismatch —
    // that distinction *is* runtime-backed today.
    if (m_traits.satisfies(argType, "Integer") && m_traits.satisfies(paramType, "Integer")) return true;
    if (m_traits.satisfies(argType, "Float") && m_traits.satisfies(paramType, "Float")) return true;

    // Recurse into compound types structurally rather than requiring exact
    // equality — e.g. `[Int]` vs `[Integer]` (the relaxation above, but
    // inside a list) and `String` (= `[Char]`) vs a generic `[A]` param
    // both need this, not just bare params.
    if (auto* paramList = std::get_if<ListType>(&paramType->kind)) {
        auto* argList = std::get_if<ListType>(&argType->kind);
        return argList && argMatchesParam(argList->element, paramList->element);
    }
    if (auto* paramTuple = std::get_if<TupleType>(&paramType->kind)) {
        auto* argTuple = std::get_if<TupleType>(&argType->kind);
        if (!argTuple || argTuple->elements.size() != paramTuple->elements.size()) return false;
        for (size_t i = 0; i < paramTuple->elements.size(); i++) {
            if (!argMatchesParam(argTuple->elements[i], paramTuple->elements[i])) return false;
        }
        return true;
    }
    if (auto* paramMap = std::get_if<MapType>(&paramType->kind)) {
        auto* argMap = std::get_if<MapType>(&argType->kind);
        return argMap && argMatchesParam(argMap->key, paramMap->key) &&
               argMatchesParam(argMap->value, paramMap->value);
    }
    if (auto* paramOpt = std::get_if<OptionalType>(&paramType->kind)) {
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (auto* paramFn = std::get_if<FuncType>(&paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) return isPermissive(argType);
        if (argFn->params.size() == paramFn->params.size()) {
            for (size_t i = 0; i < paramFn->params.size(); i++) {
                if (!argMatchesParam(argFn->params[i], paramFn->params[i])) return false;
            }
            // Unit as the expected return means "result discarded" — accept any body type.
            if (auto* prim = std::get_if<PrimitiveType>(&paramFn->result->kind);
                prim && prim->kind == PrimitiveType::Unit) return true;
            return argMatchesParam(argFn->result, paramFn->result);
        }
        // Curried arg: `(A) -> (B) -> C` matches `(A, B) -> C`.
        // Haskell-style annotations write multi-arg callbacks as `B -> A -> B`;
        // the stdlib signatures use multi-param FuncType.
        if (argFn->params.size() == 1 && paramFn->params.size() > 1) {
            if (auto* inner = std::get_if<FuncType>(&argFn->result->kind)) {
                if (inner->params.size() == paramFn->params.size() - 1) {
                    if (!argMatchesParam(argFn->params[0], paramFn->params[0])) return false;
                    for (size_t i = 0; i < inner->params.size(); i++) {
                        if (!argMatchesParam(inner->params[i], paramFn->params[i + 1])) return false;
                    }
                    return argMatchesParam(inner->result, paramFn->result);
                }
            }
        }
        return false;
    }
    // NamedType with type args — e.g. `Range<Number>` param vs `Range<Integer>` arg.
    // Recurse into type arguments structurally so the inner types get the same
    // trait-relaxation treatment (argMatchesParam, not typesEqual).
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argType->kind);
        if (!argNamed || argNamed->name != paramNamed->name) return false;
        if (argNamed->typeArgs.size() != paramNamed->typeArgs.size()) return false;
        for (size_t i = 0; i < paramNamed->typeArgs.size(); i++) {
            if (!argMatchesParam(argNamed->typeArgs[i], paramNamed->typeArgs[i])) return false;
        }
        return true;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) return true;
        }
        return false;
    }

    return typesEqual(argType, paramType);
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const -> std::string {
    // A ConstrainedType param displays as its trait name ("Integer"), not
    // its placeholder var name ("T") — readers want to know what's
    // required, not the table's internal variable naming.
    auto displayType = [](const TypePtr& t) -> std::string {
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) return constrained->traitName;
        return typeToString(t);
    };
    std::string result = name + " : ";
    for (const auto& param : sig.params) {
        result += displayType(param) + " -> ";
    }
    result += displayType(sig.result);
    return result;
}

auto TypeChecker::checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                            SourceLocation loc) -> TypePtr {
    const std::vector<Signature>* stdlibSigs = m_stdlib.lookup(name);
    auto userIt = m_userSignatures.find(name);
    bool hasUser = (userIt != m_userSignatures.end());

    // When both stdlib and user define the same name, merge so user-defined
    // overloads (e.g. user's 3-param `worker`) are visible alongside stdlib ones.
    std::vector<Signature> merged;
    const std::vector<Signature>* sigs = nullptr;
    if (stdlibSigs && hasUser) {
        merged = *stdlibSigs;
        for (const auto& s : userIt->second) merged.push_back(s);
        sigs = &merged;
    } else if (stdlibSigs) {
        sigs = stdlibSigs;
    } else if (hasUser) {
        sigs = &userIt->second;
    }
    if (!sigs) {
        // Record field access: `user.name` desugars to checkCall("name", [User]).
        // Look up the field type in the record registry before giving up.
        // Resolve TypeVars first — the receiver may have been constrained by
        // a previous inference step.
        if (!argTypes.empty()) {
            auto receiver = resolve(argTypes[0]);
            if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
                auto ri = m_recordFields.find(named->name);
                if (ri != m_recordFields.end()) {
                    auto fi = ri->second.find(name);
                    if (fi != ri->second.end()) return fi->second;
                }
            }
            // Trait-bounded receiver: `item.method()` where `item: SomeTrait`.
            // Look up the method in the trait's required methods to get return type.
            if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
                if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                    for (const auto& req : trait->requiredMethods) {
                        if (req.name == name) return req.result;
                    }
                }
            }
            // If the field name is unambiguously defined on exactly one record
            // type, constrain a TypeVar receiver to that record type.
            if (std::holds_alternative<TypeVar>(receiver->kind)) {
                std::string matchedRecord;
                TypePtr matchedFieldType;
                for (const auto& [recName, fields] : m_recordFields) {
                    auto fi = fields.find(name);
                    if (fi != fields.end()) {
                        if (!matchedRecord.empty()) { matchedRecord.clear(); break; }
                        matchedRecord = recName;
                        matchedFieldType = fi->second;
                    }
                }
                if (!matchedRecord.empty()) {
                    if (auto* tv = std::get_if<TypeVar>(&receiver->kind)) {
                        unifyVar(tv->id, Type::named(matchedRecord));
                    }
                    return matchedFieldType;
                }
            }
        }
        return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)
    }

    // `let hello = makeGreeter("Hello")` is a top-level zero-arg binding
    // (every top-level `let NAME = EXPR` is a 0-param function — see
    // Parser::parseFunctionDef), and referencing it as a bare identifier
    // auto-calls it (Evaluator::autoCallZeroArgConstant). `hello("Alice")`
    // is the same idiom through a call: auto-call `hello` to get the
    // closure `makeGreeter` returned, then apply "Alice" to *that* — not
    // "call hello with 1 argument," which is what zero arity would
    // otherwise mean. Only a single, unambiguous 0-param signature
    // triggers this (an overload set with a 0-param AND non-0-param
    // signature is a real arity question, not this idiom).
    if (sigs->size() == 1 && (*sigs)[0].params.empty() && !argTypes.empty()) {
        return Type::unknown();
    }

    // `This` (the trait placeholder — see Surface syntax for declaring a
    // trait in the plan) has no substitution mechanism implemented yet, so
    // it can leak through as a literal NamedType("This") from a `param :
    // This` annotation inside a `make` block. A call involving it can't be
    // meaningfully checked against any signature here — reporting a
    // mismatch against the wrong (stdlib or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (auto* named = std::get_if<NamedType>(&argType->kind); named && named->name == "This") {
            return Type::unknown();
        }
    }

    // Namespace call heuristic: `BuiltIn.foo(x)` or any call where the
    // receiver resolves to Unknown (an undefined namespace sentinel like
    // `BuiltIn`). The UFCS desugaring adds the receiver as argTypes[0], but
    // if the receiver is Unknown and no overload matches the full arity while
    // overloads DO match with the receiver dropped, treat it as a plain
    // function call through a namespace — drop argTypes[0] and re-check.
    if (!argTypes.empty() &&
        (std::holds_alternative<UnknownType>(argTypes[0]->kind) ||
         std::holds_alternative<TypeVar>(argTypes[0]->kind))) {
        bool anyFullArityMatch = false;
        bool anyDroppedArityMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) anyFullArityMatch = true;
            if (sig.params.size() == argTypes.size() - 1) anyDroppedArityMatch = true;
        }
        if (!anyFullArityMatch && anyDroppedArityMatch) {
            return checkCall(name, {argTypes.begin() + 1, argTypes.end()}, loc);
        }
    }

    // Name collision guard: a make-block method isn't registered here at
    // all (see checkFunctionDef), so `this.modulo(...)` inside a
    // user-defined `make CustomType do let modulo(...) ... end` would
    // otherwise get checked against the *stdlib* `modulo`'s signature
    // purely because the names match — a different, unrelated function.
    // Only kick in when the receiver/first argument is itself a NamedType
    // (a record/ADT/custom-type value) — a builtin-primitive mismatch
    // (e.g. `even?('c')`) must still error normally; this only suppresses
    // the case where nothing in the known overload set could ever apply.
    if (!argTypes.empty() && std::holds_alternative<NamedType>(argTypes[0]->kind)) {
        bool anyFirstParamPlausible = false;
        bool anyConstrainedFirstParam = false;
        for (const auto& sig : *sigs) {
            if (!sig.params.empty()) {
                if (std::holds_alternative<ConstrainedType>(sig.params[0]->kind))
                    anyConstrainedFirstParam = true;
                if (argMatchesParam(argTypes[0], sig.params[0])) {
                    anyFirstParamPlausible = true;
                    break;
                }
            }
        }
        // Don't silence mismatches when a trait-bounded param exists — the
        // function IS designed for NamedTypes and a constraint violation must error.
        if (!anyFirstParamPlausible && !anyConstrainedFirstParam) return Type::unknown();
    }

    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        if (sig.params.size() != argTypes.size()) continue;
        arityMatches.push_back(&sig);

        bool allMatch = true;
        for (size_t i = 0; i < sig.params.size(); i++) {
            if (!argMatchesParam(argTypes[i], sig.params[i])) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) fullMatches.push_back(&sig);
    }

    // 5c: Pick the most-specific full match when there are several.
    // Specificity per param: concrete named/list/func type (2) > trait-constrained (1) > TypeVar/Unknown (0).
    // A signature A dominates B if A >= B at every position and > at least one.
    // If no unique winner exists, fall back to the first match (preserves previous behavior for
    // untyped overloads like pattern-clause functions where all params are TypeVars).
    if (fullMatches.size() > 1) {
        auto paramSpec = [](const TypePtr& p) -> int {
            if (std::holds_alternative<TypeVar>(p->kind) ||
                std::holds_alternative<UnknownType>(p->kind)) return 0;
            if (std::holds_alternative<ConstrainedType>(p->kind)) return 1;
            return 2;
        };
        auto dominates = [&](const Signature* a, const Signature* b) {
            bool aWins = false;
            for (size_t i = 0; i < a->params.size(); i++) {
                int sa = paramSpec(a->params[i]);
                int sb = paramSpec(b->params[i]);
                if (sb > sa) return false;
                if (sa > sb) aWins = true;
            }
            return aWins;
        };
        const Signature* best = nullptr;
        for (const auto* cand : fullMatches) {
            bool dominated = false;
            for (const auto* other : fullMatches) {
                if (other == cand) continue;
                if (dominates(other, cand)) { dominated = true; break; }
            }
            if (!dominated) {
                if (!best) { best = cand; }
                // Multiple undominated candidates: ambiguous, keep first
            }
        }
        if (best) {
            // Rebuild fullMatches with best first so the code below uses it
            std::vector<const Signature*> reordered = {best};
            for (const auto* s : fullMatches) if (s != best) reordered.push_back(s);
            fullMatches = std::move(reordered);
        }
    }

    if (fullMatches.size() >= 1) {
        const auto& matched = *fullMatches[0];
        // Propagate the param types back to any TypeVar arguments so that
        // unannotated params are constrained by the functions they're passed
        // into (e.g. `let f(s) = s.split(",")` constrains `s` to String).
        // Only apply when the param is concrete — skip generic params that
        // themselves contain TypeVars (e.g. `first : [A] -> A?`).
        auto typeContainsVar = [](const TypePtr& t) {
            // Shallow check: top-level TypeVar, or List/Optional/Constrained
            // wrapping a TypeVar. Deep recursion isn't worth the complexity here.
            if (std::holds_alternative<TypeVar>(t->kind)) return true;
            if (auto* lt = std::get_if<ListType>(&t->kind))
                return std::holds_alternative<TypeVar>(lt->element->kind);
            if (auto* ot = std::get_if<OptionalType>(&t->kind))
                return std::holds_alternative<TypeVar>(ot->inner->kind);
            return false;
        };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            auto resolved = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                const auto& param = matched.params[i];
                if (!typeContainsVar(param)) unifyVar(tv->id, param);
            }
        }
        return matched.result;
    }

    if (arityMatches.empty()) {
        error(loc, "`" + name + "` expects " + std::to_string((*sigs)[0].params.size()) +
              " argument(s), got " + std::to_string(argTypes.size()));
        return (*sigs)[0].result;
    }

    // Zero matches with at least one arity match: find the first
    // mismatching argument against the first arity-matching candidate for
    // the headline message, then list every candidate signature tried,
    // Elm-style.
    const Signature& first = *arityMatches[0];
    std::string detail = "different arguments";
    for (size_t i = 0; i < first.params.size(); i++) {
        if (!argMatchesParam(argTypes[i], first.params[i])) {
            auto* constrained = std::get_if<ConstrainedType>(&first.params[i]->kind);
            std::string expected = constrained ? constrained->traitName : typeToString(first.params[i]);
            detail = "argument " + std::to_string(i + 1) + " to be " + expected +
                      ", but got " + typeToString(argTypes[i]);
            break;
        }
    }

    std::string message = "`" + name + "` expects " + detail;
    for (const auto& sig : *sigs) {
        message += "\n\n" + displaySignature(name, sig);
    }
    error(loc, message);
    return arityMatches[0]->result;
}

auto TypeChecker::pushScope() -> void {
    m_scopeStack.emplace_back();
}

auto TypeChecker::popScope() -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.pop_back();
    }
}

auto TypeChecker::defineVar(const std::string& name, TypePtr type) -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.back().set(name, std::move(type));
    } else {
        m_globals.set(name, std::move(type));
    }
}

auto TypeChecker::lookupVar(const std::string& name) const -> TypePtr {
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        if (auto type = it->get(name)) return type;
    }
    return m_globals.get(name);
}

auto TypeChecker::error(SourceLocation loc, const std::string& msg) -> void {
    if (m_diagnostics) {
        m_diagnostics->push_back({Diagnostic::Level::Error, loc, msg});
    }
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected,
                               const TypePtr& actual) -> void {
    error(loc, "Type mismatch: expected " + typeToString(expected) +
          ", got " + typeToString(actual));
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::typeVar(m_nextTypeVar++);
}

auto TypeChecker::resolve(TypePtr t) const -> TypePtr {
    while (t) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) break;
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) break;
        t = it->second;
    }
    return t;
}

auto TypeChecker::unifyVar(int id, TypePtr concrete) -> void {
    if (!m_subst.count(id)) m_subst[id] = std::move(concrete);
}

} // namespace kex::semantic

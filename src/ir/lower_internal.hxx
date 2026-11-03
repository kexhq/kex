#pragma once

// Free helpers shared by the lowering implementation, in kex::ir::detail so a
// future split of the Lowering struct across translation units does not copy
// them. These were anonymous-namespace functions at the top of lower.cxx.

#include "ir.hxx"
#include "lower.hxx"
#include "../ast/function_groups.hxx"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace kex::ir::detail {

// Backend-local symbol construction lives here so every producer and reader
// changes together. Dots identify qualified/module membership; slash
// introduces a receiver/argument overload signature.
inline auto mangleQualifiedMember(const std::string& scope,
                                  const std::string& member) -> std::string {
    return scope + "." + member;
}

inline auto mangleReceiverImplementation(const std::string& method,
                                         const std::string& receiverType)
    -> std::string {
    return method + "/" + receiverType;
}

inline auto mangleReceiverSignature(const std::string& method,
                                    const std::vector<std::string>& dispatchTypes)
    -> std::string {
    std::string out = method + "/";
    for (size_t i = 0; i < dispatchTypes.size(); i++) {
        if (i) out += ",";
        out += dispatchTypes[i];
    }
    return out;
}

inline auto receiverImplementationPrefix(const std::string& method)
    -> std::string {
    return method + "/";
}

// `kex_intrinsic_process:invoke_slot/2` dispatches into a `serving` type's
// own compiled module by looking up the slot's BARE name/arity at runtime
// (`erlang:function_exported/3` + `erlang:apply/3`) — it has no type
// information to resolve a receiver-qualified call the way ordinary Kex call
// sites do. A slot whose name collides program-wide with some other type's
// method or record field is mangled to `name/TypeName` by the usual
// collision handling, and `lowerModules`'s by-name redistribution then
// carries the one surviving bare-name definition off to whichever OTHER
// module's colliding name won the race — not this `serving` type's own
// module. So the very first zero-argument call into a slot whose name
// collides (kexhq/kex#384; kexhq/kex#377 covered only the case of a
// top-level, moduleless `serving` block) died `function not exported`, once
// anything pulling in a same-named colliding definition (`Net.HTTP`'s
// `Port.value`, in the reported case) was anywhere in the build. Called only
// AFTER `lowerModules` has finished redistributing and merging — so this
// plain wrapper, republishing the slot under its own bare name via a local
// call to the mangled implementation already sitting in the same module,
// never competes with that shared machinery for the name at all.
inline auto servingSlotBareAlias(const std::string& bareName, int arity,
                                 const std::string& targetName) -> FunDef {
    FunDef alias;
    alias.name = bareName;
    alias.arity = arity;
    FunClause clause;
    std::vector<ExprPtr> args;
    for (int i = 0; i < arity; ++i) {
        const auto argName = "_servingSlotArg" + std::to_string(i);
        auto param = std::make_unique<Pattern>();
        param->kind = PatKind::Var;
        param->name = argName;
        clause.params.push_back(std::move(param));
        args.push_back(var(argName));
    }
    auto body = std::make_unique<Expr>();
    body->node = Call{"", targetName, arity, std::move(args), false};
    clause.body = std::move(body);
    alias.clauses.push_back(std::move(clause));
    return alias;
}

inline auto mangleModulePath(const std::string& path) -> std::string {
    return path;
}

inline auto mangleModuleMember(const std::string& path,
                               const std::string& member) -> std::string {
    return mangleQualifiedMember(mangleModulePath(path), member);
}

// How many parameters the rendered spelling of a function type takes: the
// number of TOP-LEVEL arrows, so `(Env) -> String` is 1 and `(A) -> (B) -> C`
// is 2. A Kex lambda is a fun of exactly that arity at runtime, which is what
// separates `Env -> String` from the parameterless `Block<String>`.
inline auto functionTypeArity(const std::string& written) -> int64_t {
    int depth = 0;
    int64_t arrows = 0;
    for (size_t i = 0; i < written.size(); ++i) {
        const char c = written[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
        else if (c == '>' && !(i && written[i - 1] == '-')) --depth;
        else if (depth == 0 && c == '-' && i + 1 < written.size() &&
                 written[i + 1] == '>') { ++arrows; ++i; }
    }
    return arrows;
}

inline auto localOverloadKey(const std::string& method,
                             const std::string& receiverType,
                             size_t arity) -> std::string {
    return method + "\n" + receiverType + "\n" + std::to_string(arity);
}

// Does `expr` reference the SSA name `name` anywhere? Names are freshened at
// binding time, so a plain syntactic scan is exact — nothing shadows.
inline auto mentionsVar(const Expr& expr, const std::string& name) -> bool {
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

} // namespace kex::ir::detail

// Module-assembly helpers in kex::ir, shared by lower.cxx and the lowering
// TUs. Moved verbatim from lowering.hxx's tail in the Phase 1 split;
// `static` became `inline` so every TU shares one instance (a `static`
// mutable global would have silently diverged per TU).
namespace kex::ir {

// A make-block method's BEAM arity: the AST parameter count, plus 1 for the
// implicit `this` UNLESS the first param is itself the receiver (an `@`/record/
// range pattern). So `count(@[])` is count/1 but `count(pred)` is count/2 even
// though both have one AST param. Grouping and trait-inheritance both key on
// this — a method can be overloaded by BEAM arity across those two forms.
// Every name a pattern binds, in source order.
inline auto patternBoundNames(const ast::PatternPtr& p,
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


inline auto beamArity(const ast::FunctionDef* fd) -> size_t {
    return fd ? ast::methodArity(*fd) : 1;
}

inline auto unguardedCatchAll(const FunClause& c) -> bool {
    return !c.guard &&
        std::all_of(c.params.begin(), c.params.end(),
            [](const PatternPtr& p){ return p->kind == PatKind::Var || p->kind == PatKind::Wild; });
}

// Merge duplicate function definitions (same name + arity) by concatenating
// their clauses. The prelude legitimately repeats a method across make blocks
// for different types with different clause patterns (e.g. optional.kex
// defines `or` for both Optional<X> and Result<X,E>); erlc needs a single
// function with all the clauses unified, not two conflicting definitions.
// Clauses arriving after an unguarded catch-all are unreachable (first
// match wins) and are dropped — this happens when overlapping traits force
// the same method body into two make blocks (e.g. Integer defines
// identity/combine for both Monoid and Group).
//
// Also the fix for kexhq/kex#360: `lowerModules` redistributes a flat
// module's functions into per-stdlib-module buckets by bare name, and two
// UNRELATED receiver implementations from two different stdlib modules can
// legitimately share a bare name + arity purely by accident (`Net.Socket.TCP`'s
// own `close`/`closed?` and the cross-module dispatcher `close`/Client and
// `Net.HTTP.WebSocket.Connection`'s own `closed?` collide this way) — landing
// two separate, unmerged definitions in the SAME bucket, which `erlc` rejects
// outright (`{key_exists, ...}` from `beam_ssa_throw`) before any code runs.
// Applying this same merge to each bucket, exactly as already happens for the
// flat module above, folds them into one correctly-ordered dispatcher instead.
//
// `ejectDispatcherConflicts`, when given, changes what happens on exactly
// ONE of those collisions: a cross-owner dispatcher (`isCollisionDispatcher`)
// landing on a free function's bare name. That combination can never merge
// (a free function's clause has no discriminating guard) and normally throws
// — correctly, since a free function and a make-block method sharing a bare
// name is always an accidental collision the author needs to rename their
// way out of (kexhq/kex#250). But a dispatcher isn't hand-written: it exists
// ONLY because the same bare name legitimately belongs to several OTHER
// owners too, and it is reachable from wherever it happens to be compiled —
// so instead of throwing, the dispatcher is moved to `*ejectDispatcherConflicts`
// (the entry module) rather than left fighting the free function for this
// bucket's copy of the name.
//
// `keepDeclaringModulesApart` leaves definitions from different declaring
// modules unmerged. `lowerModules` wants that for the flat pass, which runs
// before it redistributes: Units.SI and Units.Data both `make Measure do let
// to(String, in: …)`, and merged there, both modules' clauses went to
// whichever module the merged definition named, so Data lost its `to`.
// Each bucket is merged again once the definitions are in their modules.
inline bool g_keepDeclaringModulesApart = false;

inline auto mergeDuplicateFunctions(
    std::vector<FunDef>& functions,
    std::vector<FunDef>* ejectDispatcherConflicts = nullptr,
    bool keepDeclaringModulesApart = false) -> void {
    std::map<std::tuple<std::string, int, std::string>, FunDef> merged;
    for (auto& f : functions) {
        auto key = std::make_tuple(
            f.name, f.arity,
            keepDeclaringModulesApart ? f.declaringModule : std::string{});
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
            // A free function can never legitimately share a bare name +
            // arity with a RECEIVER method: unlike two receiver methods
            // (which may legitimately reappear across make blocks or
            // traits for different types, and are meant to be merged
            // below), there is no relationship between a free function
            // and a method that would make combining their clauses
            // correct. Concatenating or silently dropping clauses here
            // previously did whichever happened by construction order: a
            // same-shaped receiver method clause (no discriminating
            // guard, since it's the type's sole owner) looked identical
            // to a plain free function's own clause, so the free
            // function's body vanished and its callers silently invoked
            // the OTHER definition instead — worse than the "undefined
            // function" this was originally reported as (kexhq/kex#250).
            // Report the collision instead.
            //
            // Restricted to free-vs-method (not free-vs-free): the BEAM
            // REPL legitimately re-lowers an already-defined free
            // function again across reloads as the session's cumulative
            // source grows, which reaches here as two isFreeFunction
            // entries for the SAME declaration — a real duplicate there,
            // not a collision, and merging is harmless for it.
            if (existing.isFreeFunction != f.isFreeFunction) {
                // A record field's reader is in the same position as a
                // dispatcher: it was routed here by its bare name, and has
                // nothing to do with this module's function.
                auto misfiled = [](const FunDef& fn) {
                    return fn.isCollisionDispatcher || fn.isFieldAccessor;
                };
                if (ejectDispatcherConflicts &&
                    (misfiled(existing) || misfiled(f))) {
                    auto& dispatcher = misfiled(existing) ? existing : f;
                    auto& keeper = misfiled(existing) ? f : existing;
                    ejectDispatcherConflicts->push_back(std::move(dispatcher));
                    if (&keeper != &existing) existing = std::move(keeper);
                    continue;
                }
                throw LowerError(
                    "IR lower: `" + std::get<0>(key) + "/" +
                    std::to_string(std::get<1>(key)) +
                    "` names both a free function and a receiver method "
                    "in this compilation unit — rename one of them");
            }
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
    functions.clear();
    for (auto& [_, f] : merged) functions.push_back(std::move(f));
}

} // namespace kex::ir

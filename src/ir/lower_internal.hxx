#pragma once

// Free helpers shared by the lowering implementation, in kex::ir::detail so a
// future split of the Lowering struct across translation units does not copy
// them. These were anonymous-namespace functions at the top of lower.cxx.

#include "ir.hxx"

#include <cstdint>
#include <string>
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

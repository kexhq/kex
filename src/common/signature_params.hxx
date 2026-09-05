#pragma once

#include "../ast/ast.hxx"

#include <string>
#include <unordered_map>
#include <vector>

namespace kex {

// A free function's parameter types can be written two ways:
//
//   let speak(c: Cat) = ...          # on the parameter
//   speak : Cat -> String            # on a standalone signature line
//   let speak(c) = ...
//
// Only the first reached the two backends' overload dispatch, so a same-arity
// overload set written the second way silently ran whichever declaration came
// first — `speak(Dog { … })` printed "meow" (docs/issue-free-function-overloads.md).
//
// This pass closes that gap by pointing every unannotated parameter at the
// type its signature line gives it, WITHOUT touching `Param::type`: see the
// comment on `Param::signatureType` for why the two must stay separate.
//
// Make-block bodies are deliberately left alone. A method's signature may or
// may not spell its receiver (`:` vs `:>`), so peeling parameters off the
// arrow chain there is not decidable from the annotation alone — and methods
// already dispatch correctly, since a make block writes its parameter types
// inline.

// The single parameter type this expression contributes, and what remains.
// `Cat -> String` yields `Cat`, leaving `String`; anything that is not a
// function type yields nullptr (the chain is shorter than the parameter list).
inline auto peelSignatureParam(const ast::TypeExpr* type,
                               const ast::TypeExpr** rest)
    -> const ast::TypeExpr* {
    if (!type) return nullptr;
    const auto* fn = std::get_if<ast::FunctionType>(&type->kind);
    if (!fn || !fn->param) return nullptr;
    *rest = fn->result ? fn->result.get() : nullptr;
    return fn->param.get();
}

// The parameter types `annotation` supplies for a definition of `count`
// parameters, or an empty vector when the signature cannot supply that many.
inline auto signatureParamTypes(const ast::TypeExpr* annotation, size_t count)
    -> std::vector<const ast::TypeExpr*> {
    std::vector<const ast::TypeExpr*> out;
    const ast::TypeExpr* rest = annotation;
    for (size_t i = 0; i < count; ++i) {
        const ast::TypeExpr* next = nullptr;
        const auto* param = peelSignatureParam(rest, &next);
        if (!param) return {};
        out.push_back(param);
        rest = next;
    }
    return out;
}

// Attach the signature types in one body of declarations, recursing into
// nested modules and visibility/compiled blocks.
//
// An annotation stays in effect for every LATER definition of that name until
// another annotation replaces it, so the clauses of a multi-clause function
// all get the same types (and therefore still look like ONE signature to
// dispatch), while a second overload picks up its own.
//
// Several signature lines may also describe a SINGLE definition — `FS.File`
// writes one `open` four times, once per mode, over one `foul open(path,
// mode)`. Those are a contract, not an overload set, and the last one is not
// the parameter's type; a stack of them for the same undefined name is
// therefore left unattached rather than guessed at.
template <typename Items>
inline auto attachSignatureParamTypes(Items& items) -> void {
    struct Pending {
        const ast::TypeAnnotation* annotation = nullptr;
        int count = 0;      // signature lines written before the definition
        bool applied = false; // a definition of this name has been seen since
    };
    std::unordered_map<std::string, Pending> pending;
    for (auto& item : items) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                if (!node || !node->type) return;
                // `:>` / `::>` leave the receiver (or the sender) out of the
                // written chain, so the arrow segments no longer line up with
                // the parameter list one for one.
                if (node->implicitThis || node->implicitFrom) return;
                auto& entry = pending[node->name];
                if (entry.annotation && !entry.applied) { ++entry.count; return; }
                entry = Pending{node.get(), 1, false};
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (!node) return;
                auto found = pending.find(node->name);
                if (found == pending.end() || !found->second.annotation) return;
                found->second.applied = true;
                if (found->second.count != 1) return;
                const auto* annotation = found->second.annotation->type.get();
                for (auto& clause : node->clauses) {
                    if (clause.params.empty()) continue;
                    const auto types =
                        signatureParamTypes(annotation, clause.params.size());
                    if (types.empty()) continue;
                    for (size_t i = 0; i < clause.params.size(); ++i)
                        if (!clause.params[i].type || !*clause.params[i].type)
                            clause.params[i].signatureType = types[i];
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) attachSignatureParamTypes(node->body);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (node) attachSignatureParamTypes(node->items);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                if (node) attachSignatureParamTypes(node->items);
            }
        }, item);
    }
}

// The type overload dispatch should read for a parameter: the one written on
// the parameter itself, else the one its signature line supplies. Returns
// nullptr when neither exists.
inline auto dispatchParamType(const ast::Param& param) -> const ast::TypeExpr* {
    if (param.type && *param.type) return param.type->get();
    return param.signatureType;
}

} // namespace kex

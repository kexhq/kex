#include "lower.hxx"
#include "lower_internal.hxx"
#include "lowering.hxx"
#include "../ast/function_groups.hxx"
#include "../semantic/declared_type.hxx"
#include "../common/utf8.hxx"
#include "../common/well_known.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/signature_params.hxx"
#include "../common/named_arguments.hxx"
#include "../common/source_name.hxx"
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

using namespace kex::ir::detail;

namespace {

// Whether the prelude also answers `name` as a receiver method, seen from a
// module other than the prelude itself. Such a name keeps the guarded
// receiver dispatcher, whose last clause falls back to `kex_prelude:name`.
auto preludeAnswers(const Lowering& L, const std::string& module,
                    const std::string& name) -> bool {
    if (!L.externalModules || module == names::kPreludeModule) return false;
    auto providers = L.externalModules->receiverFunctions.find(name);
    if (providers == L.externalModules->receiverFunctions.end()) return false;
    return std::any_of(providers->second.begin(), providers->second.end(),
                       [](const ExternalModules::ReceiverFunction& fn) {
                           return fn.moduleAtom == names::kPreludeModule;
                       });
}

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const LowerContext& context) -> Module {
    const auto& sourcePath = context.sourcePath;
    const auto* externals = context.externals;
    const auto* externalRecords = context.externalRecords;
    const auto* resolvedCalls = context.resolvedCalls;
    const bool lowersPrelude = context.lowersPrelude;
    const auto* externalVariants = context.externalVariants;
    const auto* staticTypeOfCalls = context.staticTypeOfCalls;
    const auto* expressionTypes = context.expressionTypes;
    const auto* resolvedTraits = context.resolvedTraits;
    Lowering L;
    L.sourceFile = sourcePath;
    L.externalModules = externals;
    L.resolvedCalls = resolvedCalls;
    L.hir = context.hir;
    L.resolvedTraits = resolvedTraits;
    L.lowersPrelude = lowersPrelude;
    L.staticTypeOfCalls = staticTypeOfCalls;
    L.expressionTypes = expressionTypes;
    if (externals) L.traitMethods = externals->traitMethods;
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
    L.outputModule = mod.name;

    // Traits: a `make Type, implement: T do ... end` block inherits every
    // default method (a trait `let m = ...` with a body) of each T it doesn't
    // override itself. We splice those inherited defaults into the type's
    // method group so they go through the exact same lowering/dispatch as
    // directly-defined methods (spec/traits.kex: Bot inherits shout/passing?).
    std::unordered_map<std::string, const ast::TraitDef*> traitDefs;
    kex::forEachTraitDef(prog.items, [&](const ast::TraitDef& td,
                                         const std::string& modulePath) {
        const auto identity = kex::traitIdentity(modulePath, td.name);
        traitDefs[identity] = &td;
        auto& methods = L.traitMethods[identity];
        methods.clear();
        for (const auto& entry : td.body)
            if (auto* annotation =
                    std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&entry);
                annotation && *annotation) {
                int arity = 1;
                const ast::TypeExpr* type = (*annotation)->type.get();
                while (type) {
                    const auto* fn =
                        std::get_if<ast::FunctionType>(&type->kind);
                    if (!fn) break;
                    ++arity;
                    type = fn->result.get();
                }
                methods.push_back({(*annotation)->name, arity});
            }
    });
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
    // The `using`s of each scope, by module path ("" for the file). The
    // pre-pass below collects a make block's inherited defaults before the
    // main walk has recorded a single import, so `using Ui` then `implement:
    // Shape` has to be resolved from these.
    std::unordered_map<std::string, std::set<std::string>> scopeUsings;
    kex::collectScopeUsings(prog.items, scopeUsings);
    auto inheritedDefaults = [&](const ast::MakeDef& mk, const std::string& scope) {
        std::vector<const ast::FunctionDef*> out;
        if (mk.implements.empty()) return out;
        auto own = ownMethodNames(mk);
        std::set<MethodKey> added;
        // Semantic analysis already settled each `implement:` name with
        // the checker's import order, aliases, and only/except rules;
        // without analysis (`--no-check`), resolve from the pre-pass
        // usings as before.
        const std::vector<std::string>* identities = nullptr;
        if (L.resolvedTraits)
            if (auto resolved = L.resolvedTraits->find(&mk);
                resolved != L.resolvedTraits->end())
                identities = &resolved->second;
        const auto visible = kex::usingsVisibleIn(scopeUsings, scope);
        for (size_t i = 0; i < mk.implements.size(); ++i) {
            const auto identity =
                identities && i < identities->size()
                    ? (*identities)[i]
                    : L.traitIdentityOf(mk.implements[i], scope, true,
                                        &visible);
            auto it = traitDefs.find(identity);
            if (it == traitDefs.end()) continue;
            // Kex writes a multi-clause function as separate `let`
            // declarations, so ONE trait default can be several FunctionDefs
            // sharing a key (`let repeat(0)` then `let repeat(n: Integer)`).
            // They must all come through, or the group below lowers with the
            // first clause alone and every other call raises `function_clause`
            // (kexhq/kex#261). Dedupe across traits only: `added` gains this
            // trait's keys after its body, never during it.
            std::set<MethodKey> fromThisTrait;
            for (const auto& ti : it->second->body) {
                auto* f = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ti);
                if (!f || !*f) continue;
                MethodKey key{(*f)->name, beamArity(f->get())};
                if (own.count(key) || added.count(key)) continue;
                // Only default methods (with a body) are inherited; a bare
                // signature `describe : () -> String` carries no clause body.
                if ((*f)->clauses.empty() || (*f)->clauses[0].body.empty()) continue;
                out.push_back(f->get()); fromThisTrait.insert(key);
            }
            added.insert(fromThisTrait.begin(), fromThisTrait.end());
        }
        return out;
    };

    // Pre-pass: collect record layouts (needed for construction/field access
    // before any body is lowered) and the set of real function/method names
    // (so field accessors that would collide with them are suppressed).
    // Factored into per-kind lambdas so the flattened module items go
    // through the exact same collection as top-level items.
    std::unordered_set<std::string> definedFns;
    // Bare names of module-scoped free functions that act as receiver methods
    // for a type their own module declares (see preModuleFn).
    std::unordered_set<std::string> moduleReceiverMethods;
    // The same names keyed by the arity they are actually EMITTED with. A
    // field accessor is arity 1, so only an arity-1 function of that name
    // can own the symbol (see makeAccessors).
    std::unordered_set<std::string> definedFnArities;
    auto preRecord = [&](const ast::RecordDef& rd,
                         const std::string& owner = std::string{}) {
        L.collectRecord(rd, owner);
        L.knownTypes.insert(rd.name);
        if (!owner.empty()) L.knownTypes.insert(owner + "." + rd.name);
    };
    if (externalRecords)
        for (const auto& record : *externalRecords) {
            L.collectRecordLayout(record.name, record.fields);
            // A unit compiled apart: its defaults come as source text.
            if (!record.fieldDefaults.empty()) {
                auto& info = L.records[record.name];
                for (size_t i = 0; i < record.fieldDefaults.size() &&
                                   i < info.defaults.size(); ++i)
                    info.defaults[i] = L.importedDefault(record.fieldDefaults[i]);
                info.owner = record.declaringModule;
            }
            L.externalRecordNames.insert(record.name);
            if (!record.methodOwner.empty())
                L.externalMethodOwners[record.name] = record.methodOwner;
            L.knownTypes.insert(record.name);
        }
    // Display info only — deliberately NOT added to variantTagSet, which
    // drives codegen decisions for tags this module declares itself.
    L.staticTypeOfCalls = staticTypeOfCalls;
    if (externalVariants)
        for (const auto& variant : *externalVariants) {
            L.variantArity[variant.tag] = variant.arity;
            L.variantOwner[variant.tag] = variant.owner;
            if (!variant.methodOwner.empty())
                L.externalMethodOwners[variant.owner] = variant.methodOwner;
        }
    auto preFn = [&](const ast::FunctionDef& fd) {
        for (const auto& clause : fd.clauses)
            L.fnNamedClauses[fd.name].push_back(&clause);
        definedFns.insert(fd.name);
        if (fd.isFoul) L.foulFns.insert(fd.name);
        if (!fd.clauses.empty()) {
            const auto arity = std::to_string(fd.clauses[0].params.size());
            if (!fd.isFoul) L.pureFnArities.insert(fd.name + "/" + arity);
            else L.topLevelFoulArities.insert(fd.name + "/" + arity);
            definedFnArities.insert(fd.name + "/" + arity);
            L.definedFnArities.insert(fd.name + "/" + arity);
            L.localMethodArities.insert(fd.name + "/" + arity);
        }
        if (!fd.clauses.empty()) {
            auto pnames = ast::parameterNames(fd);
            if (pnames.empty()) L.zeroArgFns.insert(fd.name);
            for (const auto& clause : fd.clauses)
                for (const auto& p : clause.params)
                    if (p.name && !p.name->empty())
                        L.fnAllParamNames[fd.name].insert(*p.name);
            L.fnParamNames[fd.name] = std::move(pnames);
            auto defaults = ast::parameterDefaults(fd);
            if (std::any_of(defaults.begin(), defaults.end(),
                            [](auto* value) { return value != nullptr; }))
                L.fnDefaults[fd.name] = std::move(defaults);
            for (std::size_t i = 0; i < fd.clauses[0].params.size(); ++i) {
                const auto& param = fd.clauses[0].params[i];
                if (!param.type || !*param.type) continue;
                const auto* type =
                    std::get_if<ast::TypeName>(&(*param.type)->kind);
                if (!type || type->parts.empty()) continue;
                std::string written = kex::join(type->parts, ".");
                if (auto trait = L.traitIdentityOf(written, ""); !trait.empty())
                    L.fnTraitParams[fd.name].push_back({i, std::move(trait)});
            }
        }
    };
    auto preType = [&](const ast::TypeDef& td) {
        if (!td.variants) return;
        if (td.isDistinct) L.distinctTypes.insert(td.name);
        // What an alias STANDS FOR, for the dispatch guards: a structural
        // right-hand side (`type Handler = Env -> String`), or the plain name
        // a transparent alias renames (`type Slug = String`). Either way no
        // value carries the alias's own name as a tag.
        if (!td.isDistinct && !td.leadingPipe && td.variants->size() == 1 &&
            (*td.variants)[0] &&
            (kex::isTransparentTypeAlias(td) ||
             Lowering::simpleTypeName((*td.variants)[0]).empty()))
            L.structuralTypeAliases[td.name] =
                typeText(*(*td.variants)[0], TypeSpelling::Dispatch);
        if (const auto* target = kex::aliasTarget(td))
            L.declaredAliases[td.name] = target;
        // A distinct type is erased to its backing type: it has no
        // constructors of its own to lay out.
        if (td.isDistinct || kex::isTransparentTypeAlias(td)) return;
        auto& layout = L.adtLayouts[td.name];
        layout.clear();
        for (const auto& v : *td.variants) {
            auto t = Lowering::simpleTypeName(v);
            if (t.empty()) continue;
            std::vector<semantic::StructuredType> argTypes;
            if (auto* g = std::get_if<ast::GenericType>(&v->kind))
                for (const auto& arg : g->args)
                    argTypes.push_back(arg ? semantic::structuredTypeOfDeclared(*arg)
                                           : semantic::StructuredType{"Any", {}});
            layout.emplace_back(t, std::move(argTypes));
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
    // The module the make block being collected is written in.
    std::string makeModulePath;
    std::set<std::string> makeRecordNames;
    auto preMake = [&](const ast::MakeDef& md) {
        for (const auto& typeName : Lowering::makeTargetNameList(
                 md.target, makeModulePath, makeRecordNames)) {
        std::unordered_map<std::string,
            std::map<std::string, std::vector<std::string>>>
            signaturesByNameAndArity;
        auto collectMethod = [&](const ast::FunctionDef* fd) {
            if (!fd) return;
            if (md.isServing && fd->isSlot) {
                auto& slots = L.servingSlotsByType[typeName];
                if (std::find(slots.begin(), slots.end(), fd->name) == slots.end())
                    slots.push_back(fd->name);
                for (const auto& clause : fd->clauses)
                    if (clause.returnAnnotation && *clause.returnAnnotation)
                        if (const auto* named = std::get_if<ast::TypeName>(
                                &(*clause.returnAnnotation)->kind);
                            named && named->parts == std::vector<std::string>{"Void"})
                            L.servingCastSlots.insert(fd->name);
            }
            if (md.isServing && !fd->isSlot && fd->name == "upgrade")
                L.servingUpgradeTypes.insert(typeName);
            definedFns.insert(fd->name);
            // A make-block method is a definition like any other, so if it is
            // foul it takes the capability context and every call to it has
            // to pass one — including the dispatch a replaced capability
            // routes through (kexhq/kex#181).
            if (fd->isFoul) L.foulFns.insert(fd->name);
            definedFnArities.insert(
                fd->name + "/" + std::to_string(beamArity(fd)));
            L.definedFnArities.insert(
                fd->name + "/" + std::to_string(beamArity(fd)));
            if (!fd->isFoul)
                L.pureFnArities.insert(
                    fd->name + "/" + std::to_string(beamArity(fd)));
            if (!fd->clauses.empty()) {
                // A make-block method reaches its receiver one of two ways:
                // implicitly through `this` (params exclude it), or as its
                // first pattern parameter (`let rangeStart(x.._)`,
                // `let startsWith_(@['_' | rest])`). `beamArity` tells the two
                // apart from the first parameter. Recording both spellings
                // also recorded an arity nothing defines: a program's own
                // `make Integer` `to(String, fmt)` claimed `3.to(Float)` for
                // a local `to/2` that does not exist.
                L.localMethodArities.insert(
                    fd->name + "/" + std::to_string(beamArity(fd)));
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
                auto defaults = ast::parameterDefaults(*fd);
                if (std::any_of(defaults.begin(), defaults.end(), [](auto* d) { return d != nullptr; }))
                    L.methodDefaults[fd->name] = std::move(defaults);
                L.makeMethods[fd->name].push_back({typeName, makeModulePath, fd});
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
        for (const auto* fd : inheritedDefaults(md, makeModulePath)) collectMethod(fd);
        for (const auto& [nameAndArity, signatures] : signaturesByNameAndArity) {
            if (signatures.size() < 2) continue;
            if (!separableOverloadSignatures(signatures)) continue;
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
        }
    };
    std::function<void(const ast::ModuleDef&)> preModule;
    preModule = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        if (module.isCapability) {
            L.capabilityModules.insert(path);
            auto& members = L.capabilityMembers[path];
            for (const auto& item : module.body) {
                if (const auto* fd =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                    fd && *fd && (*fd)->isFoul)
                    members.insert((*fd)->name);
                else if (const auto* required =
                             std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
                         required && *required && (*required)->isFoul)
                    members.insert((*required)->name);
            }
        }
        std::set<std::string> declaredTypes;
        kex::collectDeclaredTypeNames(module.body, declaredTypes);
        std::set<std::string> declaredRecords;
        kex::collectDeclaredRecordNames(module.body, declaredRecords);
        auto preModuleFn = [&](const ast::FunctionDef* fd, bool isPublic = true) {
            if (!fd) return;
            const std::string emitted = mangleModuleMember(path, fd->name);
            definedFns.insert(emitted);
            // A module-scoped free function whose FIRST parameter is a type
            // this module declares is that type's receiver method: `using
            // Net.Socket` has to make `listener.closed?` reach
            // `Net.Socket.TCP.closed?`. Only local names reach the UFCS path,
            // and a module member is registered under its mangled name — so
            // moving the socket operations from Net.Socket's top level into
            // the protocol modules left every `connection.sendAll` and
            // `listener.closed?` as "Undefined method" (kexhq/kex#230).
            // Restricted to the module's OWN types so an ordinary module
            // export never becomes a receiver method by accident.
            if (!fd->clauses.empty() && !fd->clauses[0].params.empty()) {
                const auto& receiver = fd->clauses[0].params.front();
                if (receiver.type && *receiver.type) {
                    auto written = typeText(**receiver.type, TypeSpelling::Dispatch);
                    if (auto angle = written.find('<');
                        angle != std::string::npos && written.back() == '>')
                        written.resize(angle);
                    const auto dot = written.rfind('.');
                    const auto bare =
                        dot == std::string::npos ? written
                                                 : written.substr(dot + 1);
                    const auto owner = declaredTypes.count(bare) > 0;
                    if (owner) {
                        moduleReceiverMethods.insert(fd->name);
                        L.moduleReceiverNames.insert(fd->name);
                        // Reached under its BARE name now, so its foulness has
                        // to be recorded under that name too or the call goes
                        // out one argument short of the definition.
                        if (fd->isFoul) L.foulFns.insert(fd->name);
                        L.localMethodArities.insert(
                            fd->name + "/" +
                            std::to_string(fd->clauses[0].params.size()));
                    }
                }
            }
            // A capability's own members stay at their declared arity.
            if (fd->isFoul) L.foulFns.insert(emitted);
            // Also under the MANGLED name: a `using`-imported call routes
            // through moduleImports with the qualified designator
            // ("Rodolfo.get"), and callNeedsContext checks pureFnArities
            // under exactly the name it was given. Only the bare entry used
            // to be recorded, so that qualified call never matched its own
            // exemption and inherited "foul" from an unrelated same-named
            // foul make-method merged in from elsewhere — Rodolfo's pure
            // `get/2` next to Net.HTTP's foul Client.get lowered as `get/3`
            // and erlc rejected it (kexhq/kex#272).
            if (!fd->isFoul && !fd->clauses.empty()) {
                const auto arity = std::to_string(fd->clauses[0].params.size());
                // The bare entry is for a call that reaches this function
                // through a `using`, which a private one never is. Recorded
                // anyway, a private `failed/1` told the program's own foul
                // `failed` to drop its context: `undefined function failed/1`
                // (kexhq/kex#494).
                if (isPublic) L.pureFnArities.insert(fd->name + "/" + arity);
                L.pureFnArities.insert(emitted + "/" + arity);
            }
            L.moduleFunctions[path + "." + fd->name] = emitted;
            for (const auto& clause : fd->clauses)
                L.fnNamedClauses[path + "." + fd->name].push_back(&clause);
            if (!fd->clauses.empty() && fd->clauses[0].params.empty()) {
                L.moduleZeroArgFns.insert(path + "." + fd->name);
                L.emittedModuleZeroArg.insert(emitted);
            }
            if (!fd->clauses.empty()) {
                L.fnParamNames[path + "." + fd->name] = ast::parameterNames(*fd);
                auto defaults = ast::parameterDefaults(*fd);
                if (std::any_of(defaults.begin(), defaults.end(),
                                [](auto* value) { return value != nullptr; }))
                    L.fnDefaults[path + "." + fd->name] = std::move(defaults);

            }
        };
        auto preModuleType = [&](const ast::TypeDef* td) {
            if (!td) return;
            preType(*td);
            L.moduleDeclaredTypes.emplace(td->name, path);
            L.declaredTypeIdentities.insert(path + "." + td->name);
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
                if (*rd) preRecord(**rd, path);
            } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*md) {
                    makeModulePath = path;
                    makeRecordNames = declaredRecords;
                    preMake(**md);
                    makeModulePath.clear();
                    makeRecordNames.clear();
                    for (const auto& typeName :
                         Lowering::makeTargetNameList((*md)->target, path, declaredRecords))
                        if (!typeName.empty())
                            L.localTypeModules[typeName] = path;
                }
            } else if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
                preModuleType(td->get());
            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item)) {
                if (*cb) for (const auto& ci : (*cb)->items) {
                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci))
                        preModuleFn(cfd->get());
                    else if (auto* crd = std::get_if<std::unique_ptr<ast::RecordDef>>(&ci)) {
                        if (*crd) preRecord(**crd, path);
                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                        if (*cmd) {
                            makeModulePath = path;
                            makeRecordNames = declaredRecords;
                            preMake(**cmd);
                            makeModulePath.clear();
                            makeRecordNames.clear();
                        }
                    } else if (auto* ctd = std::get_if<std::unique_ptr<ast::TypeDef>>(&ci)) {
                        if (*ctd) preType(**ctd);
                    }
                }
            }
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                // Visibility changes who may name a declaration, not what it
                // is: a record in `private do` still needs its layout, or its
                // values stay bare tuples (#495).
                if (*vb) for (const auto& vi : (*vb)->items) {
                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                        preModuleFn(vfd->get(), (*vb)->isPublic);
                    else if (auto* vtd =
                                 std::get_if<std::unique_ptr<ast::TypeDef>>(
                                     &vi))
                        preModuleType(vtd->get());
                    else if (auto* vrd =
                                 std::get_if<std::unique_ptr<ast::RecordDef>>(&vi)) {
                        if (*vrd) preRecord(**vrd, path);
                    } else if (auto* vmd =
                                   std::get_if<std::unique_ptr<ast::MakeDef>>(&vi);
                               vmd && *vmd) {
                        makeModulePath = path;
                        makeRecordNames = declaredRecords;
                        preMake(**vmd);
                        makeModulePath.clear();
                        makeRecordNames.clear();
                        for (const auto& typeName :
                             Lowering::makeTargetNameList((*vmd)->target, path, declaredRecords))
                            if (!typeName.empty())
                                L.localTypeModules[typeName] = path;
                    }
                }
            } else if (auto* ed = std::get_if<std::unique_ptr<ast::ExportDecl>>(&item)) {
                if (*ed) {
                    std::string srcMod = (*ed)->module.dotted();
                    auto alias = (*ed)->alias.value_or((*ed)->module.parts.back());
                    auto nsPath = path + "." + alias;
                    // Records aren't re-namespaced the way functions are
                    // just below (each one copied to a new `moduleFunctions`
                    // key) — a record's identity is its BEAM tag, which
                    // stays whatever the declaring module gave it. Instead,
                    // teach `canonicalRecordName` the prefix rewrite: a
                    // value constructed as `nsPath.Circle` (`App.Geometry.
                    // Circle`, reached through `export Geometry` inside
                    // `module App`) really means `srcMod.Circle`
                    // (`Geometry.Circle`), the one records.count actually
                    // recognizes. `moduleAliases` already serves the exact
                    // same purpose for a `using M, as: Alias` prefix — the
                    // multi-segment key here is inert for that lookup (it
                    // only ever checks a call's single leading segment).
                    L.moduleAliases[nsPath] = srcMod;
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
        // A module's free function and its own make receiver function share
        // the module BEAM namespace. Give the free export a distinct symbol;
        // module-local imports below are built from this table.
        for (const auto& [name, owners] : L.methodOwners) {
            const auto key = path + "." + name;
            auto emitted = L.moduleFunctions.find(key);
            if (emitted == L.moduleFunctions.end()) continue;
            const auto ownerPrefix = path + ".";
            const bool sameModuleOwner = std::any_of(
                owners.begin(), owners.end(), [&](const std::string& owner) {
                    return owner.rfind(ownerPrefix, 0) == 0;
                });
            if (!sameModuleOwner) continue;
            const auto oldSymbol = emitted->second;
            const auto newSymbol = mangleModuleMember(path, name + "$free");
            emitted->second = newSymbol;
            definedFns.erase(oldSymbol);
            definedFns.insert(newSymbol);
            // What is known about the function by its symbol moves with it:
            // a foul one left `foulFns` behind and every call went out without
            // its capability context — `fetch$free/1` against a defined
            // `fetch$free/2` (kexhq/kex#492).
            if (L.foulFns.count(oldSymbol)) L.foulFns.insert(newSymbol);
            if (L.emittedModuleZeroArg.count(oldSymbol))
                L.emittedModuleZeroArg.insert(newSymbol);
            const auto renameArities = [&](auto& arities) {
                std::vector<std::string> renamed;
                for (const auto& entry : arities)
                    if (entry.rfind(oldSymbol + "/", 0) == 0)
                        renamed.push_back(newSymbol + entry.substr(oldSymbol.size()));
                for (auto& entry : renamed) arities.insert(std::move(entry));
            };
            renameArities(L.pureFnArities);
            renameArities(L.definedFnArities);
            renameArities(definedFnArities);
            for (auto& [_, value] : L.moduleFunctions)
                if (value == oldSymbol) value = newSymbol;
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
            if (*md) {
                preMake(**md);
            }
        } else if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
            if (*module) preModule(**module);
        }
    }
    // A method name the PRELUDE also answers for is a collision too, even
    // when this module defines exactly one owner. Without this, a single
    // local owner bound the bare name directly and captured every receiver:
    // `make Box do let showValue` made `"${a}-${b}"` on two Strings emit
    // `apply 'showValue'/1`, so BEAM printed `box-box` where the walker
    // printed `a-b` — silently wrong output, not a crash. `using Binary`
    // (or `using List`) reached the same local bind through an IMPORTED
    // make block and died `function_clause` instead.
    //
    // Treating the name as colliding routes it through the receiver
    // dispatcher built below, which already falls back to `kex_prelude:name`
    // for any receiver that is not one of the local owners.
    auto preludeAlsoOwns = [&](const std::string& name) {
        return preludeAnswers(L, mod.name, name);
    };
    for (const auto& [name, owners] : L.methodOwners)
        if (owners.size() > 1 || L.fieldAccessors.count(name) ||
            (owners.size() == 1 && preludeAlsoOwns(name) &&
             !L.knownTypes.count(owners.front())))
            L.collidingMethods.insert(name);  // order-free: a set membership
    if (mod.name != names::kPreludeModule)
        for (const auto& [name, owners] : L.methodOwners)
            if (L.fnParamNames.count(name)) {
                L.freeFunctionSymbols[name] = name + "$free";
                L.collidingMethods.insert(name);
            }
    // A `.method` may use the local-apply UFCS fallback iff it names a real
    // local function or a record field accessor.
    L.knownFns = definedFns;
    L.localMethods = definedFns;
    // Assigned, not merged, so the module-owned receiver methods collected in
    // preModuleFn are folded in here rather than being overwritten.
    L.localMethods.insert(moduleReceiverMethods.begin(),
                          moduleReceiverMethods.end());
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
        // Remember that this `field/1` is the SYNTHESIZED accessor rather than
        // a function someone wrote — the condition is makeAccessors' own. No
        // capability context is ever appended to it (kexhq/kex#191), and that
        // is only sound while nothing else owns the name at arity 1.
        if (!(definedFns.count(field) && definedFnArities.count(field + "/1")))
            L.accessorFields.insert(field);
    }

    // The parser already folded each function's clauses into one FunctionDef
    // (kexhq/kex#262), so every one lowers to its own FunDef.
    auto lowerFreeFunction = [&](const ast::FunctionDef& fn) {
        auto def = L.lowerFunctionGroup({&fn});
        def.name = L.prefixSymbol(def.name);
        def.isFreeFunction = true;
        mod.functions.push_back(std::move(def));
    };
    // Bare top-level expressions → one synthetic main/0, or, beside an
    // explicit `main`, spliced around its body in source order.
    std::vector<const ast::ExprPtr*> bareExprs;
    // Declaration-level `using M do ... end` bodies (top level and module
    // bodies), lowered immediately under their scoped bindings. Like the
    // walker — which runs them while elaborating, before any main-phase
    // code — these emit first, in file order: they are elaboration-time
    // code, static initializers beside `main`, not file-ordered steps.
    std::vector<ExprPtr> usingBodySteps;
    // An explicit `main`, lowered where it is written and emitted once every
    // bare expression is known: the walker runs those before and after its
    // body in source order, and BEAM used to drop them.
    struct ExplicitMain {
        FunDef def;
        FunClause clause;
        ExprPtr body;
        size_t bareBefore = 0;
    };
    std::optional<ExplicitMain> explicitMain;

    for (const auto& item : prog.items) {
        if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fdp && *fdp) {
            lowerFreeFunction(**fdp);
            continue;
        }
        if (std::holds_alternative<std::unique_ptr<ast::TypeAnnotation>>(item))
            continue;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                (void)node; // handled above
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                // Layout already collected; accessors emitted after the loop.
                (void)node;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (!node) return;
                // A union target applies the block to every member, so the
                // whole emission repeats once per name.
                for (const auto& typeName :
                     Lowering::makeTargetNameList(node->target)) {
                auto pushFn = [&](const ast::FunctionDef* fd) {
                    if (fd) mod.functions.push_back(L.lowerMakeGroup({fd}, typeName));
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
                for (const auto* fd : inheritedDefaults(*node, "")) pushFn(fd);
                }
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
                    explicitMain = ExplicitMain{std::move(def), std::move(fc),
                                                std::move(body), bareExprs.size()};
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
                    if (node) {
                        std::vector<std::string> tags;
                        if (auto constructors = kex::typeConstructors(*node)) {
                            for (const auto& constructor : *constructors) {
                                tags.push_back(constructor.name);
                                if (constructor.arity == 0)
                                    L.nullaryVariantTags.insert(
                                        constructor.name);
                            }
                        }
                        if (!tags.empty())
                            L.typeVariantTags[node->name] = std::move(tags);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) {
                    auto push = [&](const ast::FunctionDef* fd) {
                        if (!fd) return;
                        auto it = L.moduleFunctions.find(L.currentModulePath + "." + fd->name);
                        auto def = L.lowerFunctionGroup({fd}, "",
                            it == L.moduleFunctions.end() ? fd->name : it->second);
                        def.isFreeFunction = true;
                        def.declaringModule = L.currentModulePath;
                        mod.functions.push_back(std::move(def));
                    };
                    auto emitMake = [&](const ast::MakeDef* mk,
                                        const std::set<std::string>& declaredRecords) {
                        if (!mk) return;
                        for (const auto& typeName :
                             Lowering::makeTargetNameList(mk->target, L.currentModulePath, declaredRecords)) {
                        auto pushMethod = [&](const ast::FunctionDef* fd) {
                            if (!fd) return;
                            auto def = L.lowerMakeGroup({fd}, typeName);
                            def.declaringModule = L.currentModulePath;
                            mod.functions.push_back(std::move(def));
                        };
                        for (const auto& mi : mk->body) {
                            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                                pushMethod(fd->get());
                            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                                if (*vb) for (const auto& vi : (*vb)->items)
                                    if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        pushMethod(fd->get());
                        }
                        for (const auto* fd : inheritedDefaults(*mk, L.currentModulePath)) pushMethod(fd);
                        }
                    };
                    std::function<void(const ast::ModuleDef&)> lowerModuleBody;
                    lowerModuleBody = [&](const ast::ModuleDef& m) {
                        auto savedModulePath = L.currentModulePath;
                        L.currentModulePath = m.name;
                        std::set<std::string> declaredRecords;
                        kex::collectDeclaredRecordNames(m.body, declaredRecords);
                        auto savedImports = L.moduleImports;
                        auto savedExternalImports = L.externalImports;
                        auto savedAliases = L.moduleAliases;
                        auto savedUsing = L.usingModules;
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
                            // As at the top level (kex::typeConstructors):
                            // a distinct type has no tags of its own.
                            if (td->isDistinct ||
                                kex::isTransparentTypeAlias(*td))
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
                                std::string srcMod = (*ub)->module.dotted();
                                if ((*ub)->body.empty()) {
                                    L.applyUsingCore(srcMod, (*ub)->alias,
                                                     (*ub)->onlyNames,
                                                     (*ub)->exceptNames);
                                    continue;
                                }
                                // `using M do ... end`: scoped bindings, and
                                // the body runs — first, in file order,
                                // like the walker's elaboration phase. BEAM
                                // used to drop it entirely.
                                auto saved = L.snapshotUsingBindings();
                                L.applyUsingCore(srcMod, (*ub)->alias,
                                                 (*ub)->onlyNames,
                                                 (*ub)->exceptNames);
                                auto savedSubst = L.subst;
                                L.subst.clear();
                                for (const auto& e : (*ub)->body)
                                    usingBodySteps.push_back(L.lower(e));
                                L.subst = std::move(savedSubst);
                                L.restoreUsingBindings(std::move(saved));
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
                                        emitType(vtd->get());
                                    } else if (auto* vmk =
                                                   std::get_if<std::unique_ptr<
                                                       ast::MakeDef>>(&vi)) {
                                        emitMake(vmk->get(), declaredRecords);
                                    }
                                }
                            } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&bi)) {
                                emitMake(mk->get(), declaredRecords);
                            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&bi)) {
                                if (*cb) for (const auto& ci : (*cb)->items) {
                                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci)) {
                                        push(cfd->get());
                                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                                        emitMake(cmd->get(), declaredRecords);
                                    }
                                }
                            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&bi)) {
                                if (*child) lowerModuleBody(**child);
                            } else if (std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&bi) ||
                                       std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                    emitType(td->get());
                                }
                            }
                        }
                        // The names in scope HERE are what this module's
                        // record defaults were written against; a construction
                        // in another module lowers them with these restored.
                        auto noteRecordScope = [&](const ast::RecordDef& rd) {
                            auto entry = L.records.find(m.name + "." + rd.name);
                            if (entry == L.records.end()) return;
                            entry->second.imports = L.moduleImports;
                            entry->second.aliases = L.moduleAliases;
                        };
                        for (const auto& bi : m.body) {
                            if (const auto* rd =
                                    std::get_if<std::unique_ptr<ast::RecordDef>>(&bi);
                                rd && *rd)
                                noteRecordScope(**rd);
                            else if (const auto* vb = std::get_if<
                                         std::unique_ptr<ast::VisibilityBlock>>(&bi);
                                     vb && *vb)
                                for (const auto& vi : (*vb)->items)
                                    if (const auto* vrd = std::get_if<
                                            std::unique_ptr<ast::RecordDef>>(&vi);
                                        vrd && *vrd)
                                        noteRecordScope(**vrd);
                        }
                        L.moduleImports = std::move(savedImports);
                        L.externalImports = std::move(savedExternalImports);
                        L.moduleAliases = std::move(savedAliases);
                        L.usingModules = std::move(savedUsing);
                        L.currentModulePath = std::move(savedModulePath);
                    };
                    lowerModuleBody(*node);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (!node) return;
                std::string srcMod = node->module.dotted();
                if (node->body.empty()) {
                    L.applyUsingCore(srcMod, node->alias, node->onlyNames,
                                     node->exceptNames);
                    return;
                }
                // `using M do ... end`: scoped bindings, and the body
                // runs — first, in file order, like the walker's
                // elaboration phase. BEAM used to drop it entirely.
                auto saved = L.snapshotUsingBindings();
                L.applyUsingCore(srcMod, node->alias, node->onlyNames,
                                 node->exceptNames);
                auto savedSubst = L.subst;
                L.subst.clear();
                for (const auto& e : node->body)
                    usingBodySteps.push_back(L.lower(e));
                L.subst = std::move(savedSubst);
                L.restoreUsingBindings(std::move(saved));
            } else {
                throw LowerError(std::string("IR lower: unimplemented top-level item ")
                                 + typeid(T).name());
            }
        }, item);
    }

    if (explicitMain) {
        L.subst.clear();
        std::vector<ExprPtr> steps;
        for (auto& u : usingBodySteps) steps.push_back(std::move(u));
        for (size_t i = 0; i < explicitMain->bareBefore; ++i)
            steps.push_back(L.lower(*bareExprs[i]));
        steps.push_back(std::move(explicitMain->body));
        for (size_t i = explicitMain->bareBefore; i < bareExprs.size(); ++i)
            steps.push_back(L.lower(*bareExprs[i]));
        ExprPtr sequence = std::move(steps.back());
        for (size_t i = steps.size() - 1; i-- > 0;) {
            auto let = std::make_unique<Expr>();
            let->node = Let{L.fresh("S"), std::move(steps[i]), std::move(sequence)};
            sequence = std::move(let);
        }
        explicitMain->clause.body = L.withTestSummary(
            L.withDisplayInfo(std::move(sequence), mod.name));
        explicitMain->def.clauses.push_back(std::move(explicitMain->clause));
        mod.functions.push_back(std::move(explicitMain->def));
    }

    // Synthetic main/0 from bare top-level expressions, if no explicit main.
    if ((!bareExprs.empty() || !usingBodySteps.empty()) && !mod.hasMain) {
        L.subst.clear();
        FunDef def; def.name = "main"; def.arity = 0;
        FunClause fc;
        // Emit each expr as a statement; last is the value.
        // Using-body steps first (already lowered), then bare exprs —
        // with no using bodies this lowers in exactly the old order.
        const size_t usingCount = usingBodySteps.size();
        const size_t total = usingCount + bareExprs.size();
        std::function<ExprPtr(size_t)> at = [&](size_t i) -> ExprPtr {
            if (i < usingCount) return std::move(usingBodySteps[i]);
            return L.lower(*bareExprs[i - usingCount]);
        };
        std::function<ExprPtr(size_t)> chain = [&](size_t i) -> ExprPtr {
            if (i + 1 == total) return at(i);
            auto val = at(i);
            auto rest = chain(i + 1);
            auto let = std::make_unique<Expr>();
            let->node = Let{L.fresh("S"), std::move(val), std::move(rest)};
            return let;
        };
        fc.body = L.withTestSummary(L.withDisplayInfo(
            total == 0 ? lit(LitKind::Atom, "ok") : chain(0), mod.name));
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
    // Sorted for the same reason: `argumentOverloadSignatures` is an
    // unordered_map, and the operator dispatchers it emits land in
    // `mod.functions` in iteration order.
    std::vector<const decltype(L.argumentOverloadSignatures)::value_type*>
        overloadEntries;
    for (const auto& entry : L.argumentOverloadSignatures)
        overloadEntries.push_back(&entry);
    std::sort(overloadEntries.begin(), overloadEntries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    // The `name/Owner` argument dispatchers emitted below for named methods,
    // by name, owner and arity: the collision dispatcher may route to these.
    std::set<std::tuple<std::string, std::string, int>> namedArgumentDispatchers;
    for (const auto* overloadEntry : overloadEntries) {
        const auto& key = overloadEntry->first;
        const auto& overloads = overloadEntry->second;
        auto firstBreak = key.find('\n');
        auto lastBreak = key.rfind('\n');
        if (firstBreak == std::string::npos || firstBreak == lastBreak)
            continue;
        auto name = key.substr(0, firstBreak);
        if (name.empty()) continue;
        auto arity = std::stoi(key.substr(lastBreak + 1));
        // A named method reached through its receiver dispatcher (`to/3`,
        // which another module calls) lands on `to/Measure`, and a split
        // owner has no function of that name: its clauses went to
        // `to/Measure,Any,Integer` and `to/Measure,Any,TimeUnit`. Give it
        // one that picks between them by argument type, so `90.minute.to(
        // String, in: Hour)` from a user module reaches the TimeUnit clause
        // rather than the generic conversion.
        if (std::isalnum(static_cast<unsigned char>(name.front())) ||
            name.front() == '_') {
            const auto owner =
                key.substr(firstBreak + 1, lastBreak - firstBreak - 1);
            // Only when every overload is a function of that arity. A `foul`
            // one also takes the capability context, and one with trait
            // dictionaries takes those: call sites reach those by their
            // exact signature, and a dispatcher at the source arity would
            // name functions that do not exist.
            const bool targetsExist = std::all_of(
                overloads.begin(), overloads.end(), [&](const auto& overload) {
                    return std::any_of(
                        mod.functions.begin(), mod.functions.end(),
                        [&](const FunDef& fn) {
                            return fn.name == overload.first && fn.arity == arity;
                        });
                });
            if (!targetsExist) continue;
            namedArgumentDispatchers.emplace(name, owner, arity);
            auto def = L.makeArgumentDispatcher(
                mangleReceiverImplementation(name, owner), arity, overloads);
            // What no overload takes continues to the generic conversion,
            // as an unsplit make-block `to` does.
            const bool hasGeneric = std::any_of(
                mod.functions.begin(), mod.functions.end(),
                [&](const FunDef& fn) {
                    return fn.name == "to$generic" && fn.arity == arity;
                });
            // Inside the prelude with no generic conversion at this arity there
            // is nowhere further to go: `kex_prelude:to` is this dispatcher's
            // own caller, and forwarding there looped forever.
            if (name == "to" && (hasGeneric || mod.name != names::kPreludeModule)) {
                FunClause fallback;
                std::vector<ExprPtr> args;
                for (int i = 0; i < arity; ++i) {
                    auto param = std::make_unique<Pattern>();
                    param->kind = PatKind::Var;
                    param->name = "_a" + std::to_string(i);
                    fallback.params.push_back(std::move(param));
                    args.push_back(var("_a" + std::to_string(i)));
                }
                if (hasGeneric) {
                    auto body = std::make_unique<Expr>();
                    body->node = Call{"", "to$generic", arity,
                                      std::move(args), false};
                    fallback.body = std::move(body);
                } else {
                    fallback.body = L.callE(names::kPreludeModule, "to", arity,
                                          std::move(args));
                }
                def.clauses.push_back(std::move(fallback));
            }
            mod.functions.push_back(std::move(def));
            continue;
        }
        std::string fallbackModule;
        if (L.externalModules) {
            auto external = L.externalModules->receiverFunctions.find(name);
            if (external != L.externalModules->receiverFunctions.end()) {
                for (const auto& candidate : external->second)
                    if (candidate.beamArity == arity &&
                        candidate.beamFunction == name &&
                        candidate.moduleAtom != mod.name) {
                        fallbackModule = candidate.moduleAtom;
                        break;
                    }
                if (fallbackModule.empty())
                for (const auto& candidate : external->second) {
                    if (candidate.beamArity != arity ||
                        candidate.moduleAtom == mod.name)
                        continue;
                    if (fallbackModule.empty())
                        fallbackModule = candidate.moduleAtom;
                    else if (fallbackModule != candidate.moduleAtom) {
                        fallbackModule.clear();
                        break;
                    }
                }
            }
            if (fallbackModule.empty()) {
                const auto key = std::string(
                    semantic::kFileLevelPreludeModule) + "." + name;
                auto exported = L.externalModules->exportToBeamFn.find(key);
                auto exportedArity = L.externalModules->exportArity.find(key);
                auto prelude = L.externalModules->nameToAtom.find(
                    std::string(semantic::kFileLevelPreludeModule));
                if (exported != L.externalModules->exportToBeamFn.end() &&
                    exportedArity != L.externalModules->exportArity.end() &&
                    exportedArity->second == arity &&
                    prelude != L.externalModules->nameToAtom.end() &&
                    prelude->second != mod.name)
                    fallbackModule = prelude->second;
            }
            if (fallbackModule.empty() && name == "to" &&
                mod.name != names::kPreludeModule)
                fallbackModule = names::kPreludeModule;
        }
        mod.functions.push_back(L.makeArgumentDispatcher(
            name, arity, overloads, fallbackModule));
    }
    // Cross-type collision dispatchers (bare name → runtime-type dispatch).
    // Generated PER ARITY, including only the owner types that actually have a
    // `name/Type` variant at that BEAM arity — a method can be overloaded by arity across
    // types (e.g. `count/1` on List vs `count/2` on List+Map), and one dispatcher
    // per arity avoids referencing a variant that doesn't exist.
    // Sort the unordered set so generated Core is byte-for-byte reproducible
    // across standard-library implementations and architectures.
    std::vector<std::string> collidingNames(L.collidingMethods.begin(),
                                            L.collidingMethods.end());
    std::sort(collidingNames.begin(), collidingNames.end());
    for (const auto& name : collidingNames) {
        std::map<int, std::vector<std::string>> ownersByArity;
        std::string prefix = receiverImplementationPrefix(name);
        // A mangled implementation's suffix is either a bare receiver type
        // (`+/Set`) or, for a receiver argument-overloaded within its own
        // type (`make Set do + :> Set<A> -> ...; + :> [A] -> ...`), a
        // receiver-then-argument-types signature (`+/Set,Set<A>`) — see
        // `mangleReceiverSignature`. Taking the whole suffix as the owner
        // name for the second case fabricated non-existent owners like
        // "Set,Set<A>" (no such type, no such record tag) whose guard could
        // never match anything, so `Data.Set` silently lost its `+` on BEAM
        // while `Data.UnorderedSet`'s legitimate single-signature-looking
        // suffix happened to survive (kexhq/kex#233 follow-up, no ticket).
        //
        // Such a receiver is not missing here, though: `argumentOverloadSignatures`
        // (built above, before this loop runs) already emits a bare `name`/arity
        // function per overloaded owner, each guarding on receiver AND argument
        // type, and the final duplicate-function merge pass concatenates every
        // one of those into a single dispatcher. Re-adding the same owner here
        // — under a fabricated name calling a function that does not exist —
        // only reintroduces the bug, so an argument-overloaded owner is
        // excluded from this dispatcher entirely and left to that mechanism.
        for (const auto& fn : mod.functions) {
            if (fn.name.rfind(prefix, 0) != 0) continue;
            const auto suffix = fn.name.substr(prefix.size());
            const auto owner = suffix.substr(0, suffix.find(','));
            // A named method's split owner has a `name/Owner` when the
            // argument dispatcher above was emitted for it, and stays an owner.
            const bool named =
                std::isalnum(static_cast<unsigned char>(name.front())) ||
                name.front() == '_';
            if (L.argumentOverloadedMethods.count(
                    localOverloadKey(name, owner, fn.arity)) &&
                !(named && namedArgumentDispatchers.count({name, owner, fn.arity})))
                continue;
            auto& owners = ownersByArity[fn.arity];
            if (std::find(owners.begin(), owners.end(), owner) == owners.end())
                owners.push_back(owner);
        }
        for (const auto& [arity, owners] : ownersByArity) {
            if (arity < 1 || owners.empty()) continue;
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
            // A name the PRELUDE also answers for keeps the guarded
            // dispatcher: merging ADT clauses drops the receiver guards, and
            // a make-block method's implicit `this` is an unguarded catch-all,
            // so the merged function swallows every receiver — Strings
            // included — with no way left to reach `kex_prelude:name`.
            const bool preludeOwnsName = preludeAnswers(L, mod.name, name);
            // Merging is only wrong when one of the clauses is an UNGUARDED
            // CATCH-ALL. A method written with an implicit `this` and no
            // pattern (`make Box do let showValue = "box"`) lowers to
            // `fun (This) -> ...`, which matches a String just as happily as a
            // Box — so the merged function swallows every receiver and the
            // prelude never sees it. Clauses that are distinct variant
            // patterns have no such problem, and merging them is what lets
            // ADT dispatch happen natively.
            bool hasCatchAllClause = false;
            for (const auto& fn : mod.functions) {
                if (fn.arity != arity ||
                    fn.name.rfind(prefix, 0) != 0) continue;
                for (const auto& clause : fn.clauses) {
                    if (clause.guard) continue;
                    if (std::all_of(clause.params.begin(), clause.params.end(),
                                    [](const PatternPtr& pat) {
                                        return pat->kind == PatKind::Var ||
                                               pat->kind == PatKind::Wild;
                                    }))
                        hasCatchAllClause = true;
                }
            }
            bool allAdt = !owners.empty() && !L.freeFunctionSymbols.count(name) &&
                          !L.fieldAccessors.count(name) &&
                          !(preludeOwnsName && hasCatchAllClause);
            for (const auto& o : owners) {
                if (!L.typeVariantTags.count(o)) { allAdt = false; break; }
            }
            if (allAdt) {
                FunDef merged; merged.name = name; merged.arity = arity;
                merged.isCollisionDispatcher = true;
                // Each owner's `name/Owner` forwards into the merged function,
                // so a direct call lands where the dispatch would: all routed.
                std::vector<std::string> routes;
                for (size_t i = 0; i < mod.functions.size(); ) {
                    auto& f = mod.functions[i];
                    std::string prefix = receiverImplementationPrefix(name);
                    if (f.name.rfind(prefix, 0) == 0 && f.arity == arity) {
                        routes.push_back(f.name);
                        for (auto& c : f.clauses)
                            merged.clauses.push_back(std::move(c));
                        // The clauses now live in the merged function, but the
                        // mangled name is still what a trait dictionary binds
                        // (`makeTraitDictionaryFor` picks `name/Type` for any
                        // colliding method). Erasing it left that reference
                        // dangling, so passing such a type where a trait is
                        // expected failed in erlc with an undefined function.
                        // Keep the name as a forwarder into the merged one.
                        f.clauses.clear();
                        FunClause forward;
                        std::vector<ExprPtr> args;
                        for (int a = 0; a < arity; ++a) {
                            auto param = std::make_unique<Pattern>();
                            param->kind = PatKind::Var;
                            param->name = "_mergedArg" + std::to_string(a);
                            forward.params.push_back(std::move(param));
                            args.push_back(
                                var("_mergedArg" + std::to_string(a)));
                        }
                        auto body = std::make_unique<Expr>();
                        body->node =
                            Call{"", name, arity, std::move(args), false};
                        forward.body = std::move(body);
                        f.clauses.push_back(std::move(forward));
                    }
                    ++i;
                }
                // Merged variant clauses answer only for the local owners. A
                // name the prelude answers for as well must keep going, or a
                // receiver of any other type ends in `function_clause` where
                // the walker reaches the prelude: with `let symbol(@Px)` in
                // scope, `measure.symbol` on a Measure did exactly that.
                if (preludeOwnsName) {
                    const auto& providers =
                        L.externalModules->receiverFunctions.at(name);
                    auto provider = std::find_if(
                        providers.begin(), providers.end(),
                        [&](const ExternalModules::ReceiverFunction& fn) {
                            return fn.moduleAtom == names::kPreludeModule &&
                                   fn.beamArity == arity &&
                                   fn.beamFunction == name;
                        });
                    if (provider != providers.end()) {
                        FunClause fallback;
                        std::vector<ExprPtr> args;
                        for (int a = 0; a < arity; ++a) {
                            auto param = std::make_unique<Pattern>();
                            param->kind = PatKind::Var;
                            param->name =
                                "_preludeArg" + std::to_string(a);
                            fallback.params.push_back(std::move(param));
                            args.push_back(
                                var("_preludeArg" + std::to_string(a)));
                        }
                        fallback.body = L.callE(names::kPreludeModule, name, arity,
                                                std::move(args));
                        merged.clauses.push_back(std::move(fallback));
                    }
                }
                if (!merged.clauses.empty())
                    merged.clauses.front().routes = std::move(routes);
                mod.functions.push_back(std::move(merged));
            } else {
                // A universal top-level function may share a name/arity with
                // concrete receiver implementations. Preserve it as the last
                // runtime fallback, behind the receiver dispatcher, instead
                // of letting its catch-all clause swallow the dispatcher when
                // duplicate function definitions are merged below.
                std::string genericFallback;
                std::string genericFallbackModule;
                // A `foul` implementation's last parameter is the hidden
                // capability context, so the prelude function this dispatcher
                // falls back to lives one arity DOWN (see makeDispatcher).
                const std::string mangledPrefix =
                    receiverImplementationPrefix(name);
                // Only when EVERY owner at this arity carries the context: with
                // a pure owner beside a foul one — `Headers.get(this, name)`
                // and `Process.Shared.get` (this + context) are both get/2 —
                // the second argument of a call reaching the fallback is as
                // likely a user's key as a context, and reading it as the
                // context dropped the key: `json["version"]` on a map became
                // `kex_prelude:get(map, #{})` in any program that compiled in
                // Net.HTTP (kexhq/kex#408).
                bool contextualOwner = false;
                bool pureOwner = false;
                for (const auto& fn : mod.functions)
                    if (fn.arity == arity &&
                        fn.name.rfind(mangledPrefix, 0) == 0) {
                        if (fn.hasCapabilityContext) contextualOwner = true;
                        else pureOwner = true;
                    }
                const bool contextualOwners = contextualOwner && !pureOwner;
                // A dispatcher can mix a foul owner with a PURE prelude
                // fallback — `write` is Mock.Files' (foul, context-carrying)
                // and FileHandle's (pure) at once. Prefer the context-stripped
                // arity, but fall back to the plain one when nothing answers
                // there, or the fallback goes missing and an opaque receiver
                // dies with "Undefined method" (kexhq/kex#143).
                int fallbackArity = contextualOwners ? arity - 1 : arity;
                if (contextualOwners && L.externalModules) {
                    auto external =
                        L.externalModules->receiverFunctions.find(name);
                    const bool answersStripped =
                        external != L.externalModules->receiverFunctions.end() &&
                        std::any_of(external->second.begin(),
                                    external->second.end(),
                                    [&](const auto& candidate) {
                                        return candidate.beamArity ==
                                                   fallbackArity &&
                                               candidate.moduleAtom != mod.name;
                                    });
                    if (!answersStripped) fallbackArity = arity;
                }
                for (auto& function : mod.functions) {
                    if (function.name == L.prefixSymbol(name) &&
                        function.name != name && function.arity == arity) {
                        genericFallback = function.name;
                        break;
                    }
                    if (function.name != name || function.arity != arity)
                        continue;
                    genericFallback = name + "$generic";
                    function.name = genericFallback;
                    break;
                }
                if (genericFallback.empty() && L.externalModules) {
                    auto external =
                        L.externalModules->receiverFunctions.find(name);
                    if (external !=
                        L.externalModules->receiverFunctions.end()) {
                        for (const auto& candidate : external->second)
                            if (candidate.beamArity == fallbackArity &&
                                candidate.beamFunction == name &&
                                candidate.moduleAtom != mod.name) {
                                genericFallbackModule = candidate.moduleAtom;
                                break;
                            }
                        if (genericFallbackModule.empty())
                        for (const auto& candidate : external->second) {
                            if (candidate.beamArity != fallbackArity ||
                                candidate.moduleAtom == mod.name)
                                continue;
                            if (genericFallbackModule.empty())
                                genericFallbackModule = candidate.moduleAtom;
                            else if (genericFallbackModule !=
                                     candidate.moduleAtom) {
                                genericFallbackModule.clear();
                                break;
                            }
                        }
                        if (!genericFallbackModule.empty())
                            genericFallback = name;
                    }
                    if (genericFallback.empty()) {
                        const auto key = std::string(
                            semantic::kFileLevelPreludeModule) + "." + name;
                        auto exported =
                            L.externalModules->exportToBeamFn.find(key);
                        auto exportedArity =
                            L.externalModules->exportArity.find(key);
                        auto prelude = L.externalModules->nameToAtom.find(
                            std::string(
                                semantic::kFileLevelPreludeModule));
                        if (exported !=
                                L.externalModules->exportToBeamFn.end() &&
                            exportedArity !=
                                L.externalModules->exportArity.end() &&
                            exportedArity->second == fallbackArity &&
                            prelude !=
                                L.externalModules->nameToAtom.end() &&
                            prelude->second != mod.name) {
                            genericFallbackModule = prelude->second;
                            genericFallback = name;
                        }
                    }
                    if (genericFallback.empty() && name == "to" &&
                        mod.name != names::kPreludeModule) {
                        genericFallbackModule = names::kPreludeModule;
                        genericFallback = name;
                    }
                    // `to` was special-cased by name because the lookup above
                    // only finds MODULE exports: a receiver method like
                    // `showValue` or `empty?` is not in `exportToBeamFn`, so
                    // every one of them silently lost its fallback. Give them
                    // the same escape, keyed off the receiver-function table
                    // that does know about them.
                    if (genericFallback.empty() && preludeOwnsName) {
                        genericFallbackModule = names::kPreludeModule;
                        genericFallback = name;
                    }
                }
                // A trait's DEFAULT body is reachable without going through
                // the dispatcher just built: the interface advertises
                // `name/Trait` as the backend entry point (collect_metadata's
                // `receiverIsTrait` branch), so a call site binds to it
                // directly. That is fine for a total default, but a
                // type-directed one — `make Showable do let to(String)`, which
                // answers for `String` and nothing else — then has no way to
                // reach the generic implementation the dispatcher would have
                // fallen back to. `"50".to(Integer)` died with
                // `function_clause` where the walker fell through to the
                // universal `to`.
                //
                // Give such a default the same escape the dispatcher has.
                // Only when it lacks a total clause of its own: a default that
                // already answers for everything needs nothing, and the extra
                // clause would be unreachable. Trailing trait-dictionary
                // params are not part of the call's arity, so forward just the
                // leading `arity` of them.
                if (!genericFallback.empty()) {
                    for (auto& function : mod.functions) {
                        if (function.arity < arity) continue;
                        auto slash = function.name.find('/');
                        if (slash == std::string::npos ||
                            function.name.compare(0, slash, name) != 0)
                            continue;
                        if (!L.traitMethods.count(function.name.substr(slash + 1)))
                            continue;
                        const auto total = [](const FunClause& clause) {
                            return !clause.guard &&
                                std::all_of(clause.params.begin(),
                                            clause.params.end(),
                                            [](const PatternPtr& p) {
                                                return p->kind == PatKind::Var ||
                                                       p->kind == PatKind::Wild;
                                            });
                        };
                        if (std::any_of(function.clauses.begin(),
                                        function.clauses.end(), total))
                            continue;
                        FunClause fallback;
                        std::vector<ExprPtr> args;
                        for (int i = 0; i < function.arity; ++i) {
                            auto param = std::make_unique<Pattern>();
                            param->kind = PatKind::Var;
                            param->name = "_traitFallback" + std::to_string(i);
                            fallback.params.push_back(std::move(param));
                            if (i < arity)
                                args.push_back(var(
                                    "_traitFallback" + std::to_string(i)));
                        }
                        fallback.body = L.callE(genericFallbackModule,
                                                genericFallback, arity,
                                                std::move(args));
                        function.clauses.push_back(std::move(fallback));
                    }
                }
                mod.functions.push_back(
                    L.makeDispatcher(name, arity, owners, genericFallback,
                                     genericFallbackModule, fallbackArity,
                                     contextualOwners));
            }
        }
    }

    // Field accessors last (so definedFns is fully known).
    for (auto& acc : L.makeAccessors(definedFns, definedFnArities))
        mod.functions.push_back(std::move(acc));

    // Merge duplicate function definitions (same name + arity) by
    // concatenating their clauses — see mergeDuplicateFunctions's own doc
    // comment for why this is needed and how clause ordering is preserved.
    mergeDuplicateFunctions(mod.functions, nullptr, g_keepDeclaringModulesApart);
    // Publish the source arity of every function that takes hidden trait
    // dictionaries, for the call sites that cannot build them (see
    // makeDictionaryWrapper). Never shadows a real definition at that arity.
    {
        std::set<std::pair<std::string, int>> defined;
        for (const auto& f : mod.functions) defined.insert({f.name, f.arity});
        std::vector<FunDef> wrappers;
        for (const auto& f : mod.functions) {
            auto wrapper = L.makeDictionaryWrapper(f);
            if (!wrapper) continue;
            if (!defined.insert({wrapper->name, wrapper->arity}).second)
                continue;
            wrappers.push_back(std::move(*wrapper));
        }
        for (auto& wrapper : wrappers)
            mod.functions.push_back(std::move(wrapper));
    }
    mod.typeVariantTags = L.typeVariantTags;

    // Register record layouts when the module is loaded, not only when its
    // `main` runs: a hot reload never calls `main` again, and neither does a
    // cluster node that only received this code.
    if (auto registration = L.withDisplayInfo(lit(LitKind::Atom, "ok"), mod.name);
        !std::holds_alternative<Lit>(registration->node)) {
        FunDef onLoad;
        onLoad.name = "__kex_on_load";
        onLoad.arity = 0;
        onLoad.exported = false;
        FunClause clause;
        clause.body = std::move(registration);
        onLoad.clauses.push_back(std::move(clause));
        mod.functions.push_back(std::move(onLoad));
        mod.onLoad = "__kex_on_load";
    }

    // Publish each serving type's slots on the module that implements them —
    // the same module `Process.spawn` names (see the spawnServing lowering).
    //
    // Each such module also gets `__kex_record_default(Tag, Field)`, the
    // declared default of a served record's field, or `kex_no_default`. A
    // hot reload that adds a field to a server's state record fills it from
    // here (kex_intrinsic_process:migrate/2). A module-nested type's function
    // is routed to its own module by lowerModules, via the `@path` suffix.
    std::map<std::string, FunDef> defaultFunctions;
    for (const auto& [typeName, slots] : L.servingSlotsByType) {
        std::string owner = mod.name;
        std::string defaultFunction = "__kex_record_default";
        if (auto declaring = L.localTypeModules.find(typeName);
            declaring != L.localTypeModules.end()) {
            owner = mangleModulePath("Kex." + declaring->second);
            defaultFunction += "@" + declaring->second;
        }
        auto record = L.records.find(typeName);
        if (record == L.records.end())
            for (auto it = L.records.begin(); it != L.records.end(); ++it)
                if (it->first.size() > typeName.size() &&
                    it->first.ends_with("." + typeName))
                    record = it;
        if (record != L.records.end()) {
            auto& fn = defaultFunctions[defaultFunction];
            fn.name = defaultFunction;
            fn.arity = 2;
            const auto& info = record->second;
            for (size_t i = 0; i < info.fields.size(); i++) {
                if (i >= info.defaults.size() || !info.defaults[i] ||
                    !*info.defaults[i])
                    continue;
                FunClause clause;
                for (const auto& atom : {record->first, info.fields[i]}) {
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Lit;
                    pat->litKind = LitKind::Atom;
                    pat->litText = atom;
                    clause.params.push_back(std::move(pat));
                }
                L.subst.clear();
                clause.body = L.lowerFieldDefault(info, *info.defaults[i]);
                fn.clauses.push_back(std::move(clause));
            }
        }
        mod.servingSlots[owner][typeName] = slots;
        if (L.servingUpgradeTypes.count(typeName))
            mod.servingUpgrades[owner].insert(typeName);
    }
    for (auto& [_, fn] : defaultFunctions) {
        FunClause fallback;
        fallback.params.push_back(L.wildPat());
        fallback.params.push_back(L.wildPat());
        fallback.body = lit(LitKind::Atom, "kex_no_default");
        fn.clauses.push_back(std::move(fallback));
        mod.functions.push_back(std::move(fn));
    }
    return mod;
}

auto lowerProgramTiered(const ast::Program& prog,
                        const std::array<size_t, 5>& tierBounds,
                        const std::string& fileStem,
                        const LowerContext& context) -> Module {
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
    return lowerProgram(prog, fileStem, context);
}

namespace {

// Where a bare call should be routed, and the arity that routing is valid
// for. Name alone is not enough: two modules can define the same method name
// at different arities, and redirecting on the name would send a call to a
// module that has no such function.
struct ModuleTarget {
    std::string module;
    std::string sourceName;
};
using ModuleTargets = std::unordered_map<std::string, ModuleTarget>;
// (module, name, arity) actually emitted, for the modules built in THIS unit.
// A redirect to one of them is only valid if it lands on a function that
// exists: `using Units.SI` publishes a `to` that exists ONLY at arity 3, and
// redirecting every `to`/2 in the program to it produced `undef` on BEAM and
// an empty string on the walker for `42.to(String)`. Modules compiled
// elsewhere are absent here and are trusted, as before.
using ModuleDefined = std::unordered_map<std::string,
                                         std::set<std::pair<std::string, int>>>;
// Where a call goes when its module redirect is rejected: the provider that
// really exports that name at that arity (the prelude, in practice). Without
// it a rejected redirect leaves the call bare and it resolves to nothing —
// `42.to(String)` under `using Units.SI` became `undefined function to/2`.
using ArityFallback =
    std::map<std::pair<std::string, int>, ModuleTarget>;

// `local` is what the calling module itself defines, for the entry module
// only: its functions keep the flat program's names, so one defined there at
// the call's name and arity IS the function the call was lowered against. A
// dispatcher ejected into the entry module by mergeDuplicateFunctions is the
// case that needs it — Net.HTTP's `close/2` for `make Client` lost its bucket
// to the TCP `close` the bare-name winner names, and every `client.close` in
// the program was then redirected to `Kex.Net.Socket.TCP:close`, which
// badargs on a Client (kexhq/kex#454).
auto rewriteModuleCalls(ExprPtr& expr, const ModuleTargets& targets,
                        const ModuleDefined& defined,
                        const ArityFallback& fallback,
                        const std::set<std::pair<std::string, int>>* local = nullptr)
    -> void {
    if (!expr) return;
    if (auto* call = std::get_if<Call>(&expr->node)) {
        auto& node = *call;
        if (node.module.empty() && local &&
            local->count({node.name, node.arity})) {
            // Defined right here: stays a local call.
        } else if (node.module.empty()) {
            if (auto it = targets.find(node.name); it != targets.end()) {
                auto built = defined.find(it->second.module);
                if (built == defined.end() ||
                    built->second.count({it->second.sourceName, node.arity})) {
                    node.module = it->second.module;
                    node.name = it->second.sourceName;
                } else if (auto external =
                               fallback.find({node.name, node.arity});
                           external != fallback.end()) {
                    node.module = external->second.module;
                    node.name = external->second.sourceName;
                }
            }
        } else if (auto built = defined.find(node.module);
                   built != defined.end() &&
                   !built->second.count({node.name, node.arity})) {
            // Already addressed to a module built HERE that turns out not
            // to define it. A trait dictionary aims at the concrete
            // type's owner module — right for a type that implements the
            // method, wrong when the implementation is the trait default:
            // `Kex.Units.SI:showValue/1` against a module with no
            // `showValue` at all, i.e. `undef` for any program that says
            // `using Units.SI`.
            if (auto external = fallback.find({node.name, node.arity});
                external != fallback.end()) {
                node.module = external->second.module;
                node.name = external->second.sourceName;
            }
        }
    }
    forEachChild(*expr, [&](ExprPtr& child) {
        rewriteModuleCalls(child, targets, defined, fallback, local);
    });
}

} // namespace

auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const LowerContext& context) -> std::vector<Module> {
    const auto* externals = context.externals;
    const auto* externalRecords = context.externalRecords;
    g_keepDeclaringModulesApart = true;
    struct Restore {
        ~Restore() { g_keepDeclaringModulesApart = false; }
    } restore;
    auto flat = lowerProgram(prog, fileStem, context);

    struct Definition {
        std::string path;
        std::string sourceName;
        bool exported;
        // External record accessors are already compiled in their owner
        // module (not necessarily `Kex.<path>`).
        std::string beamModule;
        // Arity this name was recorded at, or -1 when the source does not
        // pin one (record accessors). A call is only redirected to the
        // owning module when the arities agree — `using Units.SI` brings a
        // `to` that exists ONLY at arity 3, and routing every `to`/2 in the
        // program to it made `42.to(String)` an `undef` on BEAM and an empty
        // string on the walker.
        int arity = -1;
        // Claimed by a module's free function over its own type (the
        // kexhq/kex#230 bare-name redirect) rather than by a `make` method.
        bool freeOverOwnType = false;
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
        //
        // Top-level definitions of this program count the same way, whether a
        // function or a `let` constant: `let value = thunk()` binds the name
        // here, and routing a bare `value` to the prelude's Measure accessor
        // called `kex_prelude:value/0`, which does not exist — `undef` on
        // BEAM where the walker printed the constant (examples/closures.kex).
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
                } else if constexpr (std::is_constructible_v<
                                         std::decay_t<decltype(item)>,
                                         std::unique_ptr<ast::VisibilityBlock>>) {
                    if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                        vb && *vb)
                        for (const auto& vi : (*vb)->items) {
                            if (auto* vrd = std::get_if<std::unique_ptr<ast::RecordDef>>(&vi)) {
                                if (*vrd) noteRecord(**vrd);
                            } else if (auto* vmk = std::get_if<std::unique_ptr<ast::MakeDef>>(&vi)) {
                                if (*vmk) noteMake(**vmk);
                            }
                        }
                }
            }
            (void)self;
        };
        scanModule = [&](const ast::ModuleDef& module) {
            scanItems(module.body, scanItems);
        };
        scanItems(prog.items, scanItems);
        // Top-level functions and `let` constants (the latter collected into
        // the synthetic main block) — Program-only item kinds, so they cannot
        // ride along in the shared module/program scan above.
        for (const auto& item : prog.items) {
            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fd) localMethodNames.insert((*fd)->name);
            } else if (auto* mb = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
                if (!*mb || !(*mb)->synthetic) continue;
                for (const auto& e : (*mb)->body) {
                    auto* le = std::get_if<ast::LetExpr>(&e->kind);
                    if (!le || !le->pattern) continue;
                    std::vector<std::string> names;
                    patternBoundNames(le->pattern, names);
                    for (auto& name : names)
                        localMethodNames.insert(std::move(name));
                }
            }
        }

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
    // A `serving` block's slots, declared inside a `module X do ... end` (or
    // header-style `module X`) rather than at the top level — collected here
    // so a slot whose bare name lost the collision race below (see
    // `servingSlotBareAlias`'s own comment) can still be republished under
    // its bare name in its OWN module afterward. Top-level, moduleless
    // `serving` blocks are unaffected: `topLevelServingSlots` below already
    // keeps those out of the by-name redistribution entirely.
    struct NestedServingSlot {
        std::string modulePath;
        std::string typeName;
        std::string slotName;
    };
    std::vector<NestedServingSlot> nestedServingSlots;
    std::function<void(const ast::ModuleDef&)> collect;
    collect = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        if (seenModulePaths.insert(path).second)
            modulePaths.push_back(path);
        std::set<std::string> ownTypes;
        kex::collectDeclaredTypeNames(module.body, ownTypes);
        auto add = [&](const ast::FunctionDef* fn, bool exported) {
            if (!fn) return;
            definitions[mangleModuleMember(path, fn->name)] =
                {path, fn->name, exported};
            // A module-scoped free function over one of the module's OWN types
            // is that type's receiver method, and a UFCS call reaches it under
            // its bare name — so the bare name has to redirect here too, the
            // way a make-block method's does just below. Without it
            // `listener.closed?` compiled to a local `closed?/2` that no
            // module defines (kexhq/kex#230).
            if (fn->clauses.empty() || fn->clauses[0].params.empty()) return;
            const auto& receiver = fn->clauses[0].params.front();
            if (!receiver.type || !*receiver.type) return;
            auto written = typeText(**receiver.type, TypeSpelling::Dispatch);
            if (auto angle = written.find('<');
                angle != std::string::npos && written.back() == '>')
                written.resize(angle);
            const auto dot = written.rfind('.');
            const auto bare =
                dot == std::string::npos ? written : written.substr(dot + 1);
            if (!ownTypes.count(bare)) return;
            const int arity = static_cast<int>(fn->clauses[0].params.size()) +
                              (fn->isFoul ? 1 : 0);
            if (!definitions.count(fn->name))
                definitions[fn->name] = {path, fn->name, true, "", arity, true};
        };
        auto addMake = [&](const ast::MakeDef* mk) {
            if (!mk) return;
            std::set<std::string> declaredRecords;
            kex::collectDeclaredRecordNames(module.body, declaredRecords);
            auto collectMethod = [&](const ast::FunctionDef* fd,
                                     const std::string& typeName) {
                if (!fd) return;
                // A make method's dispatcher outranks a free function's
                // bare-name claim: it falls back to the prelude for other
                // receivers, and a free function over its own type is
                // reached by its qualified symbol when the receiver's type is
                // known. First come, first served sent `client.close` to a
                // TCP `close` from a module merged earlier.
                // Several modules' `to` extensions forward to one another in
                // module-path order (see `nextExtendingModule`), so a call
                // has to enter at the first of them.
                if (auto existing = definitions.find(fd->name);
                    existing == definitions.end() ||
                    existing->second.freeOverOwnType ||
                    (fd->name == "to" && path < existing->second.path))
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
                if (mk->isServing && fd->isSlot)
                    nestedServingSlots.push_back(
                        {path, typeName, fd->name});
            };
            for (const auto& typeName :
                 Lowering::makeTargetNameList(mk->target, path, declaredRecords)) {
                for (const auto& mi : mk->body) {
                    if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                        collectMethod(fd->get(), typeName);
                    else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                        if (*vb) for (const auto& vi : (*vb)->items)
                            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                collectMethod(fd->get(), typeName);
                }
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
                    else if (auto* rd =
                                 std::get_if<std::unique_ptr<ast::RecordDef>>(&entry))
                        addRecord(rd->get());
                    else if (auto* mk =
                                 std::get_if<std::unique_ptr<ast::MakeDef>>(&entry))
                        addMake(mk->get());
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

    ModuleTargets targets;
    for (const auto& [emitted, def] : definitions)
        targets[emitted] = {def.beamModule.empty() ? "Kex." + def.path : def.beamModule,
                            def.sourceName};

    // Serving slots stay in the module that declares them. The distribution
    // below routes a function to whichever module `definitions` says owns its
    // NAME, and a slot's name is ordinary — `add` is also `Net.HTTP.Headers`'s.
    // So compiling a program that merely imports Net.HTTP moved the top-level
    // `add` slot into Net.HTTP's module, and the serving process, which
    // applies `Module:add(State, …)` against its OWN module, got
    // `{'function not exported', {…, add, 3}}` (rodolfo docs/kex-issues.md #1).
    std::unordered_set<std::string> topLevelServingSlots;
    for (const auto& item : prog.items)
        if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
            make && *make && (*make)->isServing)
            for (const auto& member : (*make)->body)
                if (const auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&member);
                    fn && *fn && (*fn)->isSlot)
                    topLevelServingSlots.insert((*fn)->name);

    std::vector<Module> result;
    std::unordered_map<std::string, std::vector<FunDef>> moduleBuckets;
    std::vector<FunDef> globalFunctions;
    for (auto& fn : flat.functions) {
        auto found = definitions.find(fn.name);
        if (found == definitions.end() ||
            topLevelServingSlots.count(fn.name)) {
            globalFunctions.push_back(std::move(fn));
            continue;
        }
        const auto& def = found->second;
        // `definitions[bareName]` can only remember ONE winner for a bare
        // name — the module whose registration happened to run last. When
        // THIS function actually belongs to a DIFFERENT module (stamped at
        // the point it was lowered, while its own `module X do ... end`
        // path was still known), routing it to the winner's bucket by bare
        // name alone would misfile someone else's `close`/`closed?` next to
        // it: two unrelated definitions, unmergeable, in one Core Erlang
        // module (kexhq/kex#360). It still belongs under its OWN bare name —
        // nothing else in its own module's bucket should also claim that
        // name — just in its own module rather than the bare-name winner's.
        if (!fn.declaringModule.empty() && fn.declaringModule != def.path) {
            moduleBuckets[fn.declaringModule].push_back(std::move(fn));
            continue;
        }
        fn.name = def.sourceName;
        fn.exported = def.exported;
        moduleBuckets[def.path].push_back(std::move(fn));
    }
    // `__kex_record_default@Store` belongs in `Kex.Store`, as the plain
    // `__kex_record_default` (see lowerProgram).
    for (auto it = globalFunctions.begin(); it != globalFunctions.end();) {
        const std::string prefix = "__kex_record_default@";
        if (it->name.rfind(prefix, 0) == 0) {
            auto path = it->name.substr(prefix.size());
            it->name = "__kex_record_default";
            moduleBuckets[path].push_back(std::move(*it));
            it = globalFunctions.erase(it);
        } else {
            ++it;
        }
    }
    flat.functions = std::move(globalFunctions);
    // Whatever stayed in the entry module was kept apart by declaring module
    // above; here it is one module again.
    mergeDuplicateFunctions(flat.functions);

    std::vector<Module> stdlibModules;
    for (const auto& path : modulePaths) {
        Module module;
        module.name = "Kex." + path;
        if (auto it = moduleBuckets.find(path); it != moduleBuckets.end()) {
            module.functions = std::move(it->second);
            // Two functions from UNRELATED stdlib modules can land in the
            // same bucket sharing a bare name + arity purely by accident: the
            // redistribution above routes by bare name alone, and a name
            // this module's own free function owns (registered so a UFCS
            // call reaches it directly) can coincide with a same-named,
            // same-arity receiver method or cross-module dispatcher that
            // merely happens to also carry that bare name (kexhq/kex#360).
            // Left unmerged, both land in this module's Core Erlang output
            // as separate definitions and `erlc` rejects the module
            // outright, before any code runs. A dispatcher that loses this
            // fight is ejected into the flat/entry module instead of
            // throwing — see mergeDuplicateFunctions's own doc comment.
            mergeDuplicateFunctions(module.functions, &flat.functions);
        }
        stdlibModules.push_back(std::move(module));
    }
    // A nested `serving` slot whose bare name lost the collision race above
    // (see `servingSlotBareAlias`'s own comment, and kexhq/kex#384) has no
    // bare-name definition left in its own module — only the mangled
    // `name/TypeName` implementation, redistributed here by declaring
    // module rather than by name. Republish it under its bare name, in that
    // same module, purely for `kex_intrinsic_process:invoke_slot/2`'s
    // runtime lookup. A slot that kept its bare name (no collision at all)
    // is left alone.
    for (const auto& slot : nestedServingSlots) {
        const auto beamModuleName = "Kex." + slot.modulePath;
        auto moduleIt = std::find_if(
            stdlibModules.begin(), stdlibModules.end(),
            [&](const Module& m) { return m.name == beamModuleName; });
        if (moduleIt == stdlibModules.end()) continue;
        const bool hasBare = std::any_of(
            moduleIt->functions.begin(), moduleIt->functions.end(),
            [&](const FunDef& fn) { return fn.name == slot.slotName; });
        if (hasBare) continue;
        const auto mangled =
            mangleReceiverImplementation(slot.slotName, slot.typeName);
        auto implIt = std::find_if(
            moduleIt->functions.begin(), moduleIt->functions.end(),
            [&](const FunDef& fn) { return fn.name == mangled; });
        if (implIt == moduleIt->functions.end()) continue;
        moduleIt->functions.push_back(
            servingSlotBareAlias(slot.slotName, implIt->arity, mangled));
    }
    // Built AFTER ejection above so a dispatcher moved into the flat module
    // at the last minute is still a valid target for a call rewritten below.
    ModuleTargets globalTargets;
    for (const auto& fn : flat.functions)
        globalTargets[fn.name] = {flat.name, fn.name};
    result.push_back(std::move(flat));
    for (auto& module : stdlibModules) {
        module.servingSlots = result.front().servingSlots;
        module.servingUpgrades = result.front().servingUpgrades;
        result.push_back(std::move(module));
    }

    // What each module in this unit actually ends up defining, so a redirect
    // can be checked against it rather than trusting the name alone.
    ModuleDefined defined;
    for (const auto& module : result)
        for (const auto& fn : module.functions)
            defined[module.name].insert({fn.name, fn.arity});

    // The provider each name/arity really comes from, dispatcher preferred.
    ArityFallback fallback;
    if (externals)
        for (const auto& [name, candidates] : externals->receiverFunctions)
            for (const auto& candidate : candidates) {
                auto key = std::make_pair(name, candidate.beamArity);
                if (candidate.beamFunction == name || !fallback.count(key))
                    fallback[key] = {candidate.moduleAtom,
                                     candidate.beamFunction};
            }

    for (size_t moduleIndex = 0; moduleIndex < result.size(); moduleIndex++) {
        auto& module = result[moduleIndex];
        const auto* local =
            moduleIndex == 0 ? &defined[module.name] : nullptr;
        for (auto& fn : module.functions)
            for (auto& clause : fn.clauses) {
                if (clause.guard)
                    rewriteModuleCalls(*clause.guard, targets, defined,
                                       fallback, local);
                rewriteModuleCalls(clause.body, targets, defined, fallback,
                                   local);
                if (moduleIndex > 0) {
                    if (clause.guard)
                        rewriteModuleCalls(*clause.guard, globalTargets,
                                           defined, fallback);
                    rewriteModuleCalls(clause.body, globalTargets, defined,
                                       fallback);
                }
            }
    }
    return result;
}

} // namespace kex::ir

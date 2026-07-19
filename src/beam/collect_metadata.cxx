#include "collect_metadata.hxx"
#include <unordered_set>

namespace kex::beam {

namespace {

auto convertTypeExprImpl(const kex::ast::TypeExpr& te) -> KexiTypePtr {
    return std::visit([&te](const auto& node) -> KexiTypePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, kex::ast::TypeName>) {
            std::string name;
            for (size_t i = 0; i < node.parts.size(); i++) {
                if (i) name += ".";
                name += node.parts[i];
            }
            if (name == "Integer" || name == "Bool" || name == "Char" ||
                name == "String" || name == "Unit" || name == "Atom" ||
                name == "Float" || name == "Number" || name == "Byte" ||
                name == "Int8" || name == "Int16" || name == "Int32" ||
                name == "Int64" || name == "UInt8" || name == "UInt16" ||
                name == "UInt32" || name == "UInt64" ||
                name == "Float32" || name == "Float64")
                return kexiPrimitive(name);
            return kexiNamed(name);
        } else if constexpr (std::is_same_v<T, kex::ast::GenericType>) {
            std::string name;
            for (size_t i = 0; i < node.name.parts.size(); i++) {
                if (i) name += ".";
                name += node.name.parts[i];
            }
            std::vector<KexiTypePtr> args;
            for (const auto& a : node.args) args.push_back(convertTypeExprImpl(*a));
            return kexiNamed(name, std::move(args));
        } else if constexpr (std::is_same_v<T, kex::ast::FunctionType>) {
            // Unroll right-nested arrows `A -> B -> C` into multi-arg
            // `Func<[A, B], C>`, matching the typechecker's
            // annotationToSignature convention. Kex source writes
            // multi-arg function types curried (right-associative), but
            // every value-level signature in KexI is uncurried.
            std::vector<KexiTypePtr> params;
            if (node.param) params.push_back(convertTypeExprImpl(*node.param));
            const ast::TypeExpr* cur = node.result.get();
            while (cur) {
                if (auto* ft = std::get_if<ast::FunctionType>(&cur->kind)) {
                    if (ft->param) params.push_back(convertTypeExprImpl(*ft->param));
                    cur = ft->result.get();
                } else {
                    break;
                }
            }
            return kexiFunc(std::move(params),
                            cur ? convertTypeExprImpl(*cur) : kexiUnknown());
        } else if constexpr (std::is_same_v<T, kex::ast::TupleType>) {
            std::vector<KexiTypePtr> elems;
            for (const auto& e : node.elements) elems.push_back(convertTypeExprImpl(*e));
            return kexiTuple(std::move(elems));
        } else if constexpr (std::is_same_v<T, kex::ast::ListType>) {
            return kexiList(convertTypeExprImpl(*node.element));
        } else if constexpr (std::is_same_v<T, kex::ast::MapType>) {
            return kexiMap(convertTypeExprImpl(*node.key),
                           convertTypeExprImpl(*node.value));
        } else if constexpr (std::is_same_v<T, kex::ast::UnionType>) {
            // Flatten union into a list
            std::vector<KexiTypePtr> members;
            members.push_back(convertTypeExprImpl(*node.left));
            members.push_back(convertTypeExprImpl(*node.right));
            return kexiUnion(std::move(members));
        } else if constexpr (std::is_same_v<T, kex::ast::OptionalType>) {
            return kexiOptional(convertTypeExprImpl(*node.inner));
        } else if constexpr (std::is_same_v<T, kex::ast::GenericVar>) {
            return kexiNamed(node.name);
        } else if constexpr (std::is_same_v<T, kex::ast::BlockType>) {
            return kexiFunc({}, convertTypeExprImpl(*node.inner));
        } else if constexpr (std::is_same_v<T, kex::ast::AtomType>) {
            return kexiPrimitive("Atom");
        }
        return kexiUnknown();
    }, te.kind);
}

auto convertTypeExpr(const kex::ast::TypeExprPtr& te) -> KexiTypePtr {
    if (!te) return kexiUnknown();
    return convertTypeExprImpl(*te);
}

auto convertSemanticType(const kex::semantic::TypePtr& type) -> KexiTypePtr {
    if (!type) return kexiUnknown();
    return std::visit([](const auto& node) -> KexiTypePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, kex::semantic::PrimitiveType>) {
            switch (node.kind) {
                case kex::semantic::PrimitiveType::Integer: return kexiPrimitive("Integer");
                case kex::semantic::PrimitiveType::Char: return kexiPrimitive("Char");
                case kex::semantic::PrimitiveType::Bool: return kexiPrimitive("Bool");
                case kex::semantic::PrimitiveType::Atom: return kexiPrimitive("Atom");
                case kex::semantic::PrimitiveType::Unit: return kexiPrimitive("Unit");
            }
        } else if constexpr (std::is_same_v<T, kex::semantic::SizedIntType>) {
            return kexiPrimitive(std::string(node.isSigned ? "Int" : "UInt") +
                                 std::to_string(node.bits));
        } else if constexpr (std::is_same_v<T, kex::semantic::SizedFloatType>) {
            return kexiPrimitive("Float" + std::to_string(node.bits));
        } else if constexpr (std::is_same_v<T, kex::semantic::NamedType>) {
            std::vector<KexiTypePtr> args;
            for (const auto& arg : node.typeArgs)
                args.push_back(convertSemanticType(arg));
            return kexiNamed(node.name, std::move(args));
        } else if constexpr (std::is_same_v<T, kex::semantic::FuncType>) {
            std::vector<KexiTypePtr> params;
            for (const auto& param : node.params)
                params.push_back(convertSemanticType(param));
            return kexiFunc(std::move(params), convertSemanticType(node.result));
        } else if constexpr (std::is_same_v<T, kex::semantic::TupleType>) {
            std::vector<KexiTypePtr> elements;
            for (const auto& element : node.elements)
                elements.push_back(convertSemanticType(element));
            return kexiTuple(std::move(elements));
        } else if constexpr (std::is_same_v<T, kex::semantic::ListType>) {
            return kexiList(convertSemanticType(node.element));
        } else if constexpr (std::is_same_v<T, kex::semantic::MapType>) {
            return kexiMap(convertSemanticType(node.key),
                           convertSemanticType(node.value));
        } else if constexpr (std::is_same_v<T, kex::semantic::OptionalType>) {
            return kexiOptional(convertSemanticType(node.inner));
        } else if constexpr (std::is_same_v<T, kex::semantic::UnionType>) {
            std::vector<KexiTypePtr> members;
            for (const auto& member : node.members)
                members.push_back(convertSemanticType(member));
            return kexiUnion(std::move(members));
        } else if constexpr (std::is_same_v<T, kex::semantic::VoidType>) {
            return kexiNever();
        } else if constexpr (std::is_same_v<T, kex::semantic::ConstrainedType>) {
            return kexiConstrained(node.varName, node.traitName);
        }
        // Unresolved inference variables and Dynamic/Unknown remain unknown
        // at the package boundary; compiler-local IDs must not enter hashes.
        return kexiUnknown();
    }, type->kind);
}

auto methodBeamArity(const kex::ast::FunctionDef& fd) -> int {
    if (fd.clauses.empty()) return 1;
    const auto& params = fd.clauses[0].params;
    if (params.empty()) return 1;
    const auto& p0 = params[0];
    bool receiverPat = !p0.name && p0.pattern &&
        (std::holds_alternative<kex::ast::ThisPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<kex::ast::RecordPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<kex::ast::RangePattern>((*p0.pattern)->kind));
    return receiverPat
        ? static_cast<int>(params.size())
        : static_cast<int>(params.size()) + 1;
}

void collectFromFunctionDef(const kex::ast::FunctionDef& fd,
                            KexiTypeInterface& iface,
                            const kex::semantic::Analyzer* analysis) {
    KexiExport exp;
    exp.name = fd.name;
    exp.beamFunction = fd.name;
    exp.isFoul = fd.isFoul;
    if (analysis) {
        if (const auto* signatures = analysis->functionSignatures(&fd);
            signatures && !signatures->empty()) {
            const auto& signature = signatures->front();
            exp.beamArity = static_cast<int>(signature.params.size());
            for (const auto& param : signature.params)
                exp.paramTypes.push_back(convertSemanticType(param));
            exp.returnType = convertSemanticType(signature.result);
            exp.isFoul = signature.isFoul || fd.isFoul;
        }
    }
    if (!exp.returnType && !fd.clauses.empty()) {
        exp.beamArity = static_cast<int>(fd.clauses[0].params.size());
        for (const auto& p : fd.clauses[0].params) {
            if (p.type && *p.type)
                exp.paramTypes.push_back(convertTypeExpr(*p.type));
            else
                exp.paramTypes.push_back(kexiUnknown());
        }
        if (fd.clauses[0].returnAnnotation && *fd.clauses[0].returnAnnotation)
            exp.returnType = convertTypeExpr(*fd.clauses[0].returnAnnotation);
        else
            exp.returnType = kexiUnknown();
    }
    if (!fd.clauses.empty())
        for (const auto& p : fd.clauses[0].params)
            exp.paramNames.push_back(p.name ? *p.name : "");
    iface.exports.push_back(std::move(exp));
}

auto receiverFunctionFromDef(
    const kex::ast::FunctionDef& fd,
    KexiTypePtr receiverType,
    const kex::semantic::Analyzer* analysis) -> KexiMethod {
    KexiMethod method;
    method.name = fd.name;
    method.receiverType = std::move(receiverType);
    method.isFoul = fd.isFoul;
    method.beamArity = methodBeamArity(fd);
    // When the first clause uses a receiver pattern (@-pattern), the
    // semantic signature includes the receiver as param[0].  Skip it
    // since receiverType is stored separately.
    bool hasReceiverParam = !fd.clauses.empty() && !fd.clauses[0].params.empty() && [&]{
        const auto& p0 = fd.clauses[0].params[0];
        return !p0.name && p0.pattern &&
            (std::holds_alternative<kex::ast::ThisPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<kex::ast::RecordPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<kex::ast::RangePattern>((*p0.pattern)->kind));
    }();
    size_t skipParams = hasReceiverParam ? 1 : 0;

    if (analysis) {
        if (const auto* signatures = analysis->functionSignatures(&fd);
            signatures && !signatures->empty()) {
            const auto& signature = signatures->front();
            for (size_t i = skipParams; i < signature.params.size(); i++)
                method.paramTypes.push_back(convertSemanticType(signature.params[i]));
            method.returnType = convertSemanticType(signature.result);
            method.isFoul = signature.isFoul || fd.isFoul;
        }
    }
    if (!method.returnType && !fd.clauses.empty()) {
        auto& params = fd.clauses[0].params;
        for (size_t i = skipParams; i < params.size(); i++)
            method.paramTypes.push_back(params[i].type && *params[i].type
                ? convertTypeExpr(*params[i].type) : kexiUnknown());
        method.returnType = fd.clauses[0].returnAnnotation &&
                            *fd.clauses[0].returnAnnotation
            ? convertTypeExpr(*fd.clauses[0].returnAnnotation)
            : kexiUnknown();
    }
    if (!fd.clauses.empty()) {
        const auto& params = fd.clauses[0].params;
        for (size_t i = skipParams; i < params.size(); i++)
            method.paramNames.push_back(params[i].name ? *params[i].name : "");
    }
    method.beamFunction = fd.name;
    return method;
}

void addReceiverFunction(const kex::ast::FunctionDef& fd,
                         KexiTypePtr receiverType,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis) {
    auto method = receiverFunctionFromDef(fd, std::move(receiverType), analysis);
    meta.methodOwnership.push_back({method.name, method.beamFunction});
    iface.methods.push_back(std::move(method));
}

auto makeTargetName(const KexiTypePtr& type) -> std::string {
    if (!type) return "";
    if (type->kind == KexiType::Primitive || type->kind == KexiType::Named)
        return type->name;
    if (type->kind == KexiType::List && !type->typeArgs.empty())
        return "List";
    if (type->kind == KexiType::Map)
        return "Map";
    if (type->kind == KexiType::Optional)
        return "Optional";
    return "";
}

// Apply an implicit-this `:>` standalone type annotation to a method whose
// syntactic/inferred params or return are still Unknown. `sigType` is the
// raw converted KexiType from the annotation: a Func means a 1+ param
// method (`(X -> Bool) -> [X]` is a 1-param method returning [X]); a
// non-Func means a 0-param method whose return type is the annotation.
// Skips entirely when the sig's arity disagrees with the def's arity — a
// mismatch means the annotation belongs to a different overload.
void patchMethodWithSig(KexiMethod& method, const KexiTypePtr& sigType) {
    if (!sigType) return;
    if (sigType->kind == KexiType::Func) {
        size_t sigParams = sigType->typeArgs.size();
        if (sigParams != method.paramTypes.size()) return;
        if (method.returnType && method.returnType->kind == KexiType::Unknown)
            method.returnType = sigType->result;
        for (size_t i = 0; i < sigParams; i++)
            if (method.paramTypes[i]->kind == KexiType::Unknown)
                method.paramTypes[i] = sigType->typeArgs[i];
    } else {
        if (!method.paramTypes.empty()) return;
        if (method.returnType && method.returnType->kind == KexiType::Unknown)
            method.returnType = sigType;
    }
}

// Same as patchMethodWithSig but for module-level / top-level KexiExports.
// Standalone sigs at module scope use the full function type
// (`String -> Result<...>`, params and return together). Skips when the
// sig's arity disagrees with the def's actual arity.
void patchExportWithSig(KexiExport& exp, const KexiTypePtr& sigType) {
    if (!sigType) return;
    if (sigType->kind == KexiType::Func) {
        if (sigType->typeArgs.size() != exp.paramTypes.size()) return;
        if (exp.returnType && exp.returnType->kind == KexiType::Unknown)
            exp.returnType = sigType->result;
        for (size_t i = 0; i < exp.paramTypes.size(); i++)
            if (exp.paramTypes[i]->kind == KexiType::Unknown)
                exp.paramTypes[i] = sigType->typeArgs[i];
    } else {
        if (!exp.paramTypes.empty()) return;
        if (exp.returnType && exp.returnType->kind == KexiType::Unknown)
            exp.returnType = sigType;
    }
}

// Build a standalone-method KexiMethod purely from a `:>` annotation, used
// when a make/trait block declares a signature with no implementation
// (`to :> Y -> Y?`, `second :> X?`). The method has no body but its
// signature is part of the public contract.
auto methodFromSig(const std::string& name,
                   const KexiTypePtr& receiverType,
                   const KexiTypePtr& sigType) -> KexiMethod {
    KexiMethod method;
    method.name = name;
    method.receiverType = receiverType;
    method.beamFunction = name;
    method.typeOnly = true;
    method.beamArity = 1;  // receiver only; bumped if sig adds params
    if (sigType && sigType->kind == KexiType::Func) {
        for (const auto& arg : sigType->typeArgs) method.paramTypes.push_back(arg);
        method.returnType = sigType->result;
        method.beamArity = 1 + static_cast<int>(sigType->typeArgs.size());
    } else {
        method.returnType = sigType ? sigType : kexiUnknown();
    }
    return method;
}

// Build a standalone KexiExport purely from a top-level `:` annotation,
// used when a function is implemented elsewhere (e.g. as an interpreter
// builtin) but the public contract still lives in Kex source.
auto exportFromSig(const std::string& name,
                   const KexiTypePtr& sigType) -> KexiExport {
    KexiExport exp;
    exp.name = name;
    exp.beamFunction = name;
    if (sigType && sigType->kind == KexiType::Func) {
        for (const auto& arg : sigType->typeArgs) exp.paramTypes.push_back(arg);
        exp.returnType = sigType->result;
        exp.beamArity = static_cast<int>(sigType->typeArgs.size());
    } else {
        exp.returnType = sigType ? sigType : kexiUnknown();
        exp.beamArity = 0;
    }
    return exp;
}

void collectFromMakeDef(const kex::ast::MakeDef& md,
                        KexiTypeInterface& iface,
                        KexiStructuralMetadata& meta,
                        const kex::semantic::Analyzer* analysis) {
    auto receiverType = convertTypeExpr(md.target);
    auto typeName = makeTargetName(receiverType);
    for (const auto& trait : md.implements) {
        if (!typeName.empty())
            meta.traitConformances.push_back({typeName, trait});
    }
    // Collect standalone type annotations (`:>` sigs) to patch missing
    // types and to surface pure signatures with no implementation.
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : md.body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    auto addWithSigPatch = [&](const kex::ast::FunctionDef& fd) {
        addReceiverFunction(fd, receiverType, iface, meta, analysis);
        auto it = standaloneSigs.find(fd.name);
        if (it == standaloneSigs.end()) return;
        auto& method = iface.methods.back();
        size_t defParams = method.paramTypes.size();
        for (const auto& sig : it->second) {
            if (!sig) continue;
            size_t sigParams = (sig->kind == KexiType::Func) ? sig->typeArgs.size() : 0;
            if (sigParams == defParams)
                patchMethodWithSig(method, sig);
        }
    };

    std::unordered_set<std::string> seenImpls;
    for (const auto& item : md.body) {
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fd && *fd) {
            seenImpls.insert((*fd)->name);
            addWithSigPatch(**fd);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&item);
                   visibility && *visibility && (*visibility)->isPublic) {
            for (const auto& visible : (*visibility)->items)
                if (auto* fd =
                        std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&visible);
                    fd && *fd) {
                    seenImpls.insert((*fd)->name);
                    addWithSigPatch(**fd);
                }
        }
    }
    // Add pure signatures (no `let` body) as type-only methods so
    // the type checker sees them through the imported interface.
    // No methodOwnership entry — these have no BEAM implementation
    // in this module; the IR lowerer handles them via intrinsic
    // dispatch (e.g. `to` → `kex_io:to_string_optional`).
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs)
                iface.methods.push_back(methodFromSig(name, receiverType, sigType));
}

void collectFromTraitDef(const kex::ast::TraitDef& trait,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis) {
    KexiTraitDef traitMetadata;
    traitMetadata.name = trait.name;
    for (const auto& item : trait.body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann)
            traitMetadata.requiredMethods.push_back(
                {(*ann)->name, (*ann)->isFoul});
    meta.traitDefs.push_back(std::move(traitMetadata));

    auto receiverType = kexiConstrained("T", trait.name);
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : trait.body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    std::unordered_set<std::string> seenImpls;
    for (const auto& item : trait.body)
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fd && *fd) {
            seenImpls.insert((*fd)->name);
            addReceiverFunction(**fd, receiverType, iface, meta, analysis);
            auto it = standaloneSigs.find((*fd)->name);
            if (it != standaloneSigs.end())
                for (const auto& sig : it->second)
                    patchMethodWithSig(iface.methods.back(), sig);
        }
    // Add pure signatures (methods declared but not defaulted in this
    // trait) as type-only methods — no methodOwnership entry.
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs)
                iface.methods.push_back(methodFromSig(name, receiverType, sigType));
}

void collectFromTypeDef(const kex::ast::TypeDef& td, KexiTypeInterface& iface,
                        KexiStructuralMetadata& meta) {
    KexiTypeExport te;
    te.name = td.name;
    te.genericParams = td.typeParams;
    if (td.variants) {
        KexiADT adt;
        adt.name = td.name;
        adt.typeParams = td.typeParams;
        for (const auto& v : *td.variants) {
            if (!v) continue;
            if (auto* tn = std::get_if<kex::ast::TypeName>(&v->kind)) {
                if (!tn->parts.empty()) {
                    KexiConstructor ctor;
                    ctor.name = tn->parts.back();
                    ctor.tagAtom = ctor.name;
                    ctor.arity = 0;
                    adt.constructors.push_back(std::move(ctor));
                    te.constructors.push_back(tn->parts.back());
                }
            } else if (auto* gt = std::get_if<kex::ast::GenericType>(&v->kind)) {
                if (!gt->name.parts.empty()) {
                    KexiConstructor ctor;
                    ctor.name = gt->name.parts.back();
                    ctor.tagAtom = ctor.name;
                    ctor.arity = static_cast<int>(gt->args.size());
                    adt.constructors.push_back(std::move(ctor));
                    te.constructors.push_back(gt->name.parts.back());
                }
            }
        }
        if (!adt.constructors.empty())
            meta.adts.push_back(std::move(adt));
    }
    iface.types.push_back(std::move(te));
}

void collectFromRecordDef(const kex::ast::RecordDef& rd, KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta) {
    KexiTypeExport te;
    te.name = rd.name;
    te.genericParams = rd.typeParams;
    iface.types.push_back(std::move(te));

    KexiRecord rec;
    rec.name = rd.name;
    for (const auto& f : rd.fields) {
        KexiRecordField rf;
        rf.name = f.name;
        rf.type = convertTypeExpr(f.type);
        rec.fields.push_back(std::move(rf));
    }
    meta.records.push_back(std::move(rec));
}

// Collect from a ModuleDef body (ModuleItem variants). Standalone `:`
// type annotations inside the body patch the matching FunctionDef's
// Unknown param/return types; pure signatures with no implementation are
// emitted as standalone exports.
void collectFromModuleBody(const std::vector<kex::ast::ModuleItem>& body,
                           KexiTypeInterface& iface,
                           KexiStructuralMetadata& meta,
                           const kex::semantic::Analyzer* analysis) {
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    std::unordered_set<std::string> seenImpls;
    auto addFunction = [&](const kex::ast::FunctionDef& fd) {
        seenImpls.insert(fd.name);
        collectFromFunctionDef(fd, iface, analysis);
        meta.publicExports.push_back(fd.name);
        auto it = standaloneSigs.find(fd.name);
        if (it != standaloneSigs.end())
            for (const auto& sig : it->second)
                patchExportWithSig(iface.exports.back(), sig);
    };

    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                addFunction(*ptr);
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TraitDef>) {
                collectFromTraitDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta);
            }
        }, item);
    }
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs) {
                iface.exports.push_back(exportFromSig(name, sigType));
                meta.publicExports.push_back(name);
            }
}

void collectFromTopLevel(const kex::ast::Program& program,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis) {
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : program.items)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    std::unordered_set<std::string> seenImpls;
    auto addFunction = [&](const kex::ast::FunctionDef& fd) {
        seenImpls.insert(fd.name);
        collectFromFunctionDef(fd, iface, analysis);
        meta.publicExports.push_back(fd.name);
        auto it = standaloneSigs.find(fd.name);
        if (it != standaloneSigs.end())
            for (const auto& sig : it->second)
                patchExportWithSig(iface.exports.back(), sig);
    };

    for (const auto& item : program.items) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                addFunction(*ptr);
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TraitDef>) {
                collectFromTraitDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta);
            }
        }, item);
    }
    // Top-level pure signatures describe functions implemented elsewhere
    // (interpreter builtins like `assert`, `describe`, `it`, `die`).
    // Surface them so type checking against the public contract works
    // without the stdlib signature table. Multiple overloads for the same
    // name (e.g. `assert : Bool -> Bool` / `assert : Bool -> String -> Bool`)
    // each produce a separate export.
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs) {
                iface.exports.push_back(exportFromSig(name, sigType));
                meta.publicExports.push_back(name);
            }
}

void collectFlattenedModuleBody(
    const std::vector<kex::ast::ModuleItem>& body,
    const std::string& modulePath,
    KexiTypeInterface& iface,
    KexiStructuralMetadata& meta,
    const kex::semantic::Analyzer* analysis) {
    std::string emittedPrefix;
    for (char c : modulePath)
        emittedPrefix += c == '.' ? "__" : std::string(1, c);

    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    std::unordered_set<std::string> seenImpls;
    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                seenImpls.insert(ptr->name);
                collectFromFunctionDef(*ptr, iface, analysis);
                auto& exp = iface.exports.back();
                exp.name = modulePath + "." + ptr->name;
                exp.beamFunction = emittedPrefix + "__" + ptr->name;
                meta.publicExports.push_back(exp.name);
                auto it = standaloneSigs.find(ptr->name);
                if (it != standaloneSigs.end())
                    for (const auto& sig : it->second)
                        patchExportWithSig(exp, sig);
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TraitDef>) {
                collectFromTraitDef(*ptr, iface, meta, analysis);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta);
            } else if constexpr (std::is_same_v<T, kex::ast::ModuleDef>) {
                collectFlattenedModuleBody(ptr->body, ptr->name, iface, meta,
                                           analysis);
            }
        }, item);
    }
    // Pure signatures at module scope (e.g. Math constant decls).
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs) {
                auto exp = exportFromSig(name, sigType);
                exp.name = modulePath + "." + name;
                exp.beamFunction = emittedPrefix + "__" + name;
                iface.exports.push_back(std::move(exp));
                meta.publicExports.push_back(modulePath + "." + name);
            }
}

void collectFlattenedProgram(const kex::ast::Program& program,
                             KexiTypeInterface& iface,
                             KexiStructuralMetadata& meta,
                             const kex::semantic::Analyzer* analysis) {
    collectFromTopLevel(program, iface, meta, analysis);
    for (const auto& item : program.items)
        if (auto* module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module)
            collectFlattenedModuleBody((*module)->body, (*module)->name,
                                       iface, meta, analysis);
}

auto findAndCollectModuleBody(const std::vector<kex::ast::ModuleItem>& body,
                              const std::string& moduleName,
                              KexiTypeInterface& iface,
                              KexiStructuralMetadata& meta,
                              const kex::semantic::Analyzer* analysis) -> bool {
    for (const auto& item : body)
        if (auto* module =
                std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
            if ((*module)->name == moduleName) {
                collectFromModuleBody((*module)->body, iface, meta, analysis);
                meta.isFoul = (*module)->isFoul;
                return true;
            }
            if (findAndCollectModuleBody((*module)->body, moduleName, iface,
                                         meta, analysis))
                return true;
        }
    return false;
}

// Find a top-level or nested ModuleDef by its qualified source name.
auto findAndCollectModule(const kex::ast::Program& program,
                          const std::string& moduleName,
                          KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta,
                          const kex::semantic::Analyzer* analysis) -> bool {
    for (const auto& item : program.items)
        if (auto* module =
                std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
            if ((*module)->name == moduleName) {
                collectFromModuleBody((*module)->body, iface, meta, analysis);
                meta.isFoul = (*module)->isFoul;
                return true;
            }
            if (findAndCollectModuleBody((*module)->body, moduleName, iface,
                                         meta, analysis))
                return true;
        }
    return false;
}

} // namespace

auto collectMetadata(const kex::ast::Program& program,
                     const CollectOptions& opts) -> KexiChunk {
    KexiChunk chunk;
    chunk.version = KEXI_SCHEMA_VERSION;
    chunk.metadata.unitId = !opts.unitId.empty()
        ? opts.unitId
        : (opts.role == KexiModuleRole::Companion
               ? opts.entryBackPointer : opts.moduleAtom);
    chunk.metadata.sourceModule = opts.moduleName;
    chunk.metadata.moduleAtom = opts.moduleAtom;
    chunk.metadata.role = opts.role;
    chunk.metadata.entryBackPointer = opts.entryBackPointer;

    if (!opts.noCheck) {
        if (opts.flattenModules)
            collectFlattenedProgram(program, chunk.typeInterface, chunk.metadata,
                                    opts.analysis);
        else if (!opts.collectTopLevel && !opts.moduleName.empty())
            findAndCollectModule(program, opts.moduleName,
                                 chunk.typeInterface, chunk.metadata,
                                 opts.analysis);
        else
            collectFromTopLevel(program, chunk.typeInterface, chunk.metadata,
                                opts.analysis);
    } else {
        // --no-check: structural metadata only, type interface stays empty.
        for (const auto& item : program.items) {
            std::visit([&](const auto& ptr) {
                using T = std::decay_t<decltype(*ptr)>;
                if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                    KexiStructuralMetadata dummy;
                    KexiTypeInterface dummyIface;
                    collectFromTypeDef(*ptr, dummyIface, chunk.metadata);
                } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                    KexiStructuralMetadata dummy;
                    KexiTypeInterface dummyIface;
                    collectFromRecordDef(*ptr, dummyIface, chunk.metadata);
                }
            }, item);
        }
    }

    chunk.interfaceHash = computeInterfaceHash(chunk);
    return chunk;
}

} // namespace kex::beam

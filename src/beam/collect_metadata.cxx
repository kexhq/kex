#include "collect_metadata.hxx"

namespace kex::beam {

namespace {

auto convertTypeExpr(const kex::ast::TypeExprPtr& te) -> KexiTypePtr {
    if (!te) return kexiUnknown();
    return std::visit([](const auto& node) -> KexiTypePtr {
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
            for (const auto& a : node.args) args.push_back(convertTypeExpr(a));
            return kexiNamed(name, std::move(args));
        } else if constexpr (std::is_same_v<T, kex::ast::FunctionType>) {
            std::vector<KexiTypePtr> params;
            if (node.param) params.push_back(convertTypeExpr(node.param));
            return kexiFunc(std::move(params), convertTypeExpr(node.result));
        } else if constexpr (std::is_same_v<T, kex::ast::TupleType>) {
            std::vector<KexiTypePtr> elems;
            for (const auto& e : node.elements) elems.push_back(convertTypeExpr(e));
            return kexiTuple(std::move(elems));
        } else if constexpr (std::is_same_v<T, kex::ast::ListType>) {
            return kexiList(convertTypeExpr(node.element));
        } else if constexpr (std::is_same_v<T, kex::ast::MapType>) {
            return kexiMap(convertTypeExpr(node.key), convertTypeExpr(node.value));
        } else if constexpr (std::is_same_v<T, kex::ast::UnionType>) {
            // Flatten union into a list
            std::vector<KexiTypePtr> members;
            members.push_back(convertTypeExpr(node.left));
            members.push_back(convertTypeExpr(node.right));
            return kexiUnion(std::move(members));
        } else if constexpr (std::is_same_v<T, kex::ast::OptionalType>) {
            return kexiOptional(convertTypeExpr(node.inner));
        } else if constexpr (std::is_same_v<T, kex::ast::GenericVar>) {
            return kexiNamed(node.name);
        } else if constexpr (std::is_same_v<T, kex::ast::BlockType>) {
            return kexiFunc({}, convertTypeExpr(node.inner));
        } else if constexpr (std::is_same_v<T, kex::ast::AtomType>) {
            return kexiPrimitive("Atom");
        }
        return kexiUnknown();
    }, te->kind);
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
    if (analysis) {
        if (const auto* signatures = analysis->functionSignatures(&fd);
            signatures && !signatures->empty()) {
            const auto& signature = signatures->front();
            for (const auto& param : signature.params)
                method.paramTypes.push_back(convertSemanticType(param));
            method.returnType = convertSemanticType(signature.result);
            method.isFoul = signature.isFoul || fd.isFoul;
        }
    }
    if (!method.returnType && !fd.clauses.empty()) {
        for (const auto& param : fd.clauses[0].params)
            method.paramTypes.push_back(param.type && *param.type
                ? convertTypeExpr(*param.type) : kexiUnknown());
        method.returnType = fd.clauses[0].returnAnnotation &&
                            *fd.clauses[0].returnAnnotation
            ? convertTypeExpr(*fd.clauses[0].returnAnnotation)
            : kexiUnknown();
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

void collectFromMakeDef(const kex::ast::MakeDef& md,
                        KexiTypeInterface& iface,
                        KexiStructuralMetadata& meta,
                        const kex::semantic::Analyzer* analysis) {
    auto receiverType = convertTypeExpr(md.target);
    for (const auto& item : md.body) {
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fd && *fd) {
            addReceiverFunction(**fd, receiverType, iface, meta, analysis);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&item);
                   visibility && *visibility && (*visibility)->isPublic) {
            for (const auto& visible : (*visibility)->items)
                if (auto* fd =
                        std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&visible);
                    fd && *fd)
                    addReceiverFunction(**fd, receiverType, iface, meta, analysis);
        }
    }
}

void collectFromTraitDef(const kex::ast::TraitDef& trait,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis) {
    auto receiverType = kexiConstrained("T", trait.name);
    for (const auto& item : trait.body)
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fd && *fd)
            addReceiverFunction(**fd, receiverType, iface, meta, analysis);
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

// Collect from a ModuleDef body (ModuleItem variants).
void collectFromModuleBody(const std::vector<kex::ast::ModuleItem>& body,
                           KexiTypeInterface& iface,
                           KexiStructuralMetadata& meta,
                           const kex::semantic::Analyzer* analysis) {
    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                collectFromFunctionDef(*ptr, iface, analysis);
                meta.publicExports.push_back(ptr->name);
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
}

void collectFromTopLevel(const kex::ast::Program& program,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis) {
    for (const auto& item : program.items) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                collectFromFunctionDef(*ptr, iface, analysis);
                meta.publicExports.push_back(ptr->name);
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
    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                collectFromFunctionDef(*ptr, iface, analysis);
                auto& exp = iface.exports.back();
                exp.name = modulePath + "." + ptr->name;
                exp.beamFunction = emittedPrefix + "__" + ptr->name;
                meta.publicExports.push_back(exp.name);
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

// Find a ModuleDef by name and collect from its body.
auto findAndCollectModule(const kex::ast::Program& program,
                          const std::string& moduleName,
                          KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta,
                          const kex::semantic::Analyzer* analysis) -> bool {
    for (const auto& item : program.items) {
        if (auto* md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
            if (*md && (*md)->name == moduleName) {
                collectFromModuleBody((*md)->body, iface, meta, analysis);
                meta.isFoul = (*md)->isFoul;
                return true;
            }
        }
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
        else if (!opts.moduleName.empty())
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

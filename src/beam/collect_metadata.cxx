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

auto makeTargetString(const kex::ast::TypeExprPtr& t) -> std::string {
    if (!t) return "";
    if (auto* tn = std::get_if<kex::ast::TypeName>(&t->kind)) {
        std::string name;
        for (size_t i = 0; i < tn->parts.size(); i++) {
            if (i) name += ".";
            name += tn->parts[i];
        }
        return name;
    }
    if (auto* g = std::get_if<kex::ast::GenericType>(&t->kind)) {
        std::string name;
        for (size_t i = 0; i < g->name.parts.size(); i++) {
            if (i) name += ".";
            name += g->name.parts[i];
        }
        return name;
    }
    if (std::holds_alternative<kex::ast::ListType>(t->kind)) return "List";
    if (std::holds_alternative<kex::ast::MapType>(t->kind)) return "Map";
    return "";
}

void collectFromFunctionDef(const kex::ast::FunctionDef& fd,
                            KexiTypeInterface& iface,
                            const std::string& /*modulePrefix*/) {
    KexiExport exp;
    exp.name = fd.name;
    exp.isFoul = fd.isFoul;
    if (!fd.clauses.empty()) {
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

void collectFromMakeDef(const kex::ast::MakeDef& md,
                        KexiTypeInterface& iface,
                        KexiStructuralMetadata& meta,
                        const std::string& modulePrefix) {
    auto targetName = makeTargetString(md.target);
    auto receiverType = convertTypeExpr(md.target);

    for (const auto& item : md.body) {
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item)) {
            if (!*fd) continue;
            KexiMethod method;
            method.name = (*fd)->name;
            method.receiverType = receiverType;
            method.isFoul = (*fd)->isFoul;
            if (!(*fd)->clauses.empty()) {
                // +1 for the implicit @this receiver param on BEAM
                method.beamArity = static_cast<int>((*fd)->clauses[0].params.size()) + 1;
                for (const auto& p : (*fd)->clauses[0].params) {
                    if (p.type && *p.type)
                        method.paramTypes.push_back(convertTypeExpr(*p.type));
                    else
                        method.paramTypes.push_back(kexiUnknown());
                }
                if ((*fd)->clauses[0].returnAnnotation && *(*fd)->clauses[0].returnAnnotation)
                    method.returnType = convertTypeExpr(*(*fd)->clauses[0].returnAnnotation);
                else
                    method.returnType = kexiUnknown();
            }
            // BEAM function name: the lowered name for make methods
            method.beamFunction = modulePrefix.empty()
                ? targetName + "." + (*fd)->name
                : modulePrefix + "." + targetName + "." + (*fd)->name;
            iface.methods.push_back(std::move(method));

            KexiMethodOwnership mo;
            mo.methodName = (*fd)->name;
            mo.beamFunction = method.beamFunction;
            meta.methodOwnership.push_back(std::move(mo));
        } else if (auto* vb = std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&item)) {
            if (!*vb || !(*vb)->isPublic) continue;
            for (const auto& vi : (*vb)->items) {
                if (auto* vfd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&vi)) {
                    if (!*vfd) continue;
                    KexiMethod method;
                    method.name = (*vfd)->name;
                    method.receiverType = receiverType;
                    method.isFoul = (*vfd)->isFoul;
                    if (!(*vfd)->clauses.empty()) {
                        method.beamArity = static_cast<int>((*vfd)->clauses[0].params.size()) + 1;
                        for (const auto& p : (*vfd)->clauses[0].params) {
                            if (p.type && *p.type)
                                method.paramTypes.push_back(convertTypeExpr(*p.type));
                            else
                                method.paramTypes.push_back(kexiUnknown());
                        }
                        if ((*vfd)->clauses[0].returnAnnotation && *(*vfd)->clauses[0].returnAnnotation)
                            method.returnType = convertTypeExpr(*(*vfd)->clauses[0].returnAnnotation);
                        else
                            method.returnType = kexiUnknown();
                    }
                    method.beamFunction = modulePrefix.empty()
                        ? targetName + "." + (*vfd)->name
                        : modulePrefix + "." + targetName + "." + (*vfd)->name;
                    iface.methods.push_back(std::move(method));
                }
            }
        }
    }
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
                           const std::string& modulePrefix) {
    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                collectFromFunctionDef(*ptr, iface, modulePrefix);
                meta.publicExports.push_back(ptr->name);
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, modulePrefix);
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
                         const std::string& modulePrefix) {
    for (const auto& item : program.items) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                collectFromFunctionDef(*ptr, iface, modulePrefix);
                meta.publicExports.push_back(ptr->name);
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, modulePrefix);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta);
            }
        }, item);
    }
}

// Find a ModuleDef by name and collect from its body.
auto findAndCollectModule(const kex::ast::Program& program,
                          const std::string& moduleName,
                          KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta) -> bool {
    for (const auto& item : program.items) {
        if (auto* md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
            if (*md && (*md)->name == moduleName) {
                collectFromModuleBody((*md)->body, iface, meta, "");
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
    chunk.metadata.moduleAtom = opts.moduleAtom;
    chunk.metadata.role = opts.role;
    chunk.metadata.entryBackPointer = opts.entryBackPointer;

    if (!opts.noCheck) {
        if (!opts.moduleName.empty())
            findAndCollectModule(program, opts.moduleName,
                                 chunk.typeInterface, chunk.metadata);
        else
            collectFromTopLevel(program, chunk.typeInterface, chunk.metadata, "");
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

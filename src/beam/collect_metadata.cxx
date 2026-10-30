#include "../ast/function_groups.hxx"
#include <algorithm>
#include "collect_metadata.hxx"
#include "../common/type_def_utils.hxx"
#include <fstream>
#include <functional>
#include <unordered_set>

namespace kex::beam {

namespace {

// The traits the unit being collected declares, and the `using`s of each of
// its scopes — what a make block's `implement: Shape` needs to be exported
// as the identity it means (`Geo.Shape`), which an importer cannot work out
// without the scope it was written in. Set for one collectMetadata call.
struct TraitExportScope {
    std::set<std::string> identities;
    std::unordered_map<std::string, std::set<std::string>> usings;
};
thread_local const TraitExportScope* tTraitExportScope = nullptr;

// A trait this unit declares, by its identity; anything else (a stdlib
// trait, a capability) exactly as written.
auto exportedTraitName(const std::string& written, const std::string& modulePath)
    -> std::string {
    if (!tTraitExportScope) return written;
    const auto visible = kex::usingsVisibleIn(tTraitExportScope->usings, modulePath);
    const std::vector<std::string> imported(visible.begin(), visible.end());
    const auto& identities = tTraitExportScope->identities;
    return kex::resolveTraitName(
        written, modulePath, imported,
        [&](const std::string& name) { return identities.count(name) > 0; },
        [&](const auto& visit) { for (const auto& name : identities) visit(name); });
}

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
                name == "String" || name == "Void" || name == "Atom" ||
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
        } else if constexpr (std::is_same_v<T, kex::ast::IntersectionType>) {
            return kexiIntersection({convertTypeExprImpl(*node.left),
                                     convertTypeExprImpl(*node.right)});
        } else if constexpr (std::is_same_v<T, kex::ast::RecordType>) {
            std::vector<std::string> names;
            std::vector<KexiTypePtr> types;
            for (const auto& [name, fieldType] : node.fields) {
                names.push_back(name);
                types.push_back(convertTypeExprImpl(*fieldType));
            }
            return kexiRecord(std::move(names), std::move(types));
        } else if constexpr (std::is_same_v<T, kex::ast::OptionalType>) {
            return kexiOptional(convertTypeExprImpl(*node.inner));
        } else if constexpr (std::is_same_v<T, kex::ast::GenericVar>) {
            return kexiNamed(node.name);
        } else if constexpr (std::is_same_v<T, kex::ast::BlockType>) {
            std::vector<KexiTypePtr> args;
            for (const auto& param : node.params)
                args.push_back(convertTypeExprImpl(*param));
            args.push_back(convertTypeExprImpl(*node.inner));
            return kexiNamed("Block", std::move(args));
        } else if constexpr (std::is_same_v<T, kex::ast::AtomType>) {
            // ":macos", not "Atom": the literal is what makes a union of atom
            // literals readable when a consumer prints the type back.
            return kexiPrimitive(":" + node.name);
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
                case kex::semantic::PrimitiveType::String: return kexiPrimitive("String");
                case kex::semantic::PrimitiveType::Bool: return kexiPrimitive("Bool");
                case kex::semantic::PrimitiveType::Atom:
                    return kexiPrimitive(node.atomName.empty()
                                             ? "Atom" : ":" + node.atomName);
                case kex::semantic::PrimitiveType::Unit: return kexiPrimitive("Void");
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
            if (node.block) {
                std::vector<KexiTypePtr> args;
                for (const auto& param : node.params)
                    args.push_back(convertSemanticType(param));
                args.push_back(convertSemanticType(node.result));
                return kexiNamed("Block", std::move(args));
            }
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
        } else if constexpr (std::is_same_v<T, kex::semantic::IntersectionType>) {
            std::vector<KexiTypePtr> members;
            for (const auto& member : node.members)
                members.push_back(convertSemanticType(member));
            return kexiIntersection(std::move(members));
        } else if constexpr (std::is_same_v<T, kex::semantic::RecordType>) {
            std::vector<std::string> names;
            std::vector<KexiTypePtr> types;
            for (const auto& [name, fieldType] : node.fields) {
                names.push_back(name);
                types.push_back(convertSemanticType(fieldType));
            }
            return kexiRecord(std::move(names), std::move(types));
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

auto methodBeamArity(const kex::ast::FunctionDef& fd,
                     bool firstParamIsReceiver = false,
                     size_t clause = 0) -> int {
    if (fd.clauses.empty()) return 1;
    const auto& params = fd.clauses[clause].params;
    if (params.empty()) return 1;
    if (firstParamIsReceiver)
        return static_cast<int>(params.size());
    const auto& p0 = params[0];
    bool receiverPat = !p0.name && p0.pattern &&
        (std::holds_alternative<kex::ast::ThisPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<kex::ast::RecordPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<kex::ast::RangePattern>((*p0.pattern)->kind));
    return receiverPat
        ? static_cast<int>(params.size())
        : static_cast<int>(params.size()) + 1;
}

// A default as written: the bytes its expression spans in its file, "" for
// none or for one no file holds.
auto defaultSourceOf(const std::optional<kex::ast::ExprPtr>& value) -> std::string {
    std::string source;
    if (!value || !*value) return source;
    const auto& location = (*value)->location;
    if (location.startOffset < 0 || location.endOffset <= location.startOffset ||
        location.file.empty())
        return source;
    std::ifstream file{std::string(location.file), std::ios::binary};
    file.seekg(location.startOffset);
    source.resize(static_cast<size_t>(location.endOffset - location.startOffset));
    if (!file.read(source.data(), static_cast<std::streamsize>(source.size())))
        source.clear();
    return source;
}

// Each of `params` past the first `skip`, its default as written.
auto defaultSourcesOf(const std::vector<kex::ast::Param>& params, size_t skip)
    -> std::vector<std::string> {
    std::vector<std::string> sources;
    bool any = false;
    for (size_t i = skip; i < params.size(); ++i) {
        auto source = defaultSourceOf(params[i].defaultValue);
        any = any || !source.empty();
        sources.push_back(std::move(source));
    }
    if (!any) sources.clear();
    return sources;
}

// How many of `params`, past the first `skip`, end the list with a default.
auto trailingDefaults(const std::vector<kex::ast::Param>& params, size_t skip) -> int {
    int count = 0;
    for (size_t i = params.size(); i > skip && params[i - 1].defaultValue; --i) ++count;
    return count;
}

// A function folded from several typed declarations exports one entry per
// signature it declares (`clause` picks which), as separate declarations did.
void collectFromFunctionDef(const kex::ast::FunctionDef& fd,
                            KexiTypeInterface& iface,
                            const kex::semantic::Analyzer* analysis,
                            size_t clause = 0) {
    KexiExport exp;
    exp.name = fd.name;
    exp.beamFunction = fd.name;
    exp.isFoul = fd.isFoul;
    if (analysis) {
        if (const auto* signatures = analysis->functionSignatures(&fd);
            signatures && !signatures->empty()) {
            const auto& signature = clause < signatures->size()
                ? (*signatures)[clause] : signatures->front();
            exp.beamArity = static_cast<int>(signature.params.size());
            // Only the KexI TYPE follows inference here; the arity must not.
            // The emitter (`traitNameOf` in src/ir/lower.cxx) gives a
            // dictionary parameter to explicitly trait-ANNOTATED parameters
            // and to nothing else, so counting inferred constraints too made
            // the interface advertise an arity no emitted function has. The
            // annotated ones are counted in the loop below.
            for (const auto& param : signature.params)
                exp.paramTypes.push_back(convertSemanticType(param));
            exp.returnType = convertSemanticType(signature.result);
            exp.isFoul = signature.isFoul || fd.isFoul;
        }
    }
    // A standalone generic annotation may preserve a function's public
    // result relationship (`A -> A`) while an inline parameter annotation
    // carries the trait dictionary ABI. Keep the source-facing KexI type
    // constrained and the backend arity aligned with the lowered function.
    if (analysis && !fd.clauses.empty()) {
        const auto& params = fd.clauses[clause].params;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (!params[i].type || !*params[i].type) continue;
            const auto* name = std::get_if<kex::ast::TypeName>(
                &(*params[i].type)->kind);
            if (!name || name->parts.size() != 1 ||
                !analysis->traitNeedsDictionary(name->parts.front()))
                continue;
            if (i < exp.paramTypes.size())
                exp.paramTypes[i] =
                    kexiConstrained(name->parts.front(), name->parts.front());
            ++exp.beamArity;
        }
    }
    if (!exp.returnType && !fd.clauses.empty()) {
        exp.beamArity = static_cast<int>(fd.clauses[clause].params.size());
        for (const auto& p : fd.clauses[clause].params) {
            if (p.type && *p.type)
                exp.paramTypes.push_back(convertTypeExpr(*p.type));
            else
                exp.paramTypes.push_back(kexiUnknown());
        }
        if (fd.clauses[clause].returnAnnotation && *fd.clauses[clause].returnAnnotation)
            exp.returnType = convertTypeExpr(*fd.clauses[clause].returnAnnotation);
        else
            exp.returnType = kexiUnknown();
    }
    if (!fd.clauses.empty())
        for (const auto& p : fd.clauses[clause].params)
            exp.paramNames.push_back(p.name ? *p.name : "");
    if (!fd.clauses.empty()) {
        exp.defaultedParams = trailingDefaults(fd.clauses[clause].params, 0);
        exp.defaultSources = defaultSourcesOf(fd.clauses[clause].params, 0);
    }
    iface.exports.push_back(std::move(exp));
}

auto receiverFunctionFromDef(
    const kex::ast::FunctionDef& fd,
    KexiTypePtr receiverType,
    const kex::semantic::Analyzer* analysis,
    bool firstParamIsReceiver = false,
    size_t clause = 0) -> KexiMethod {
    KexiMethod method;
    std::string receiverTrait;
    if (analysis && receiverType) {
        if (receiverType->kind == KexiType::Constrained)
            receiverTrait = receiverType->traitName;
        else if (receiverType->kind == KexiType::Named &&
                 analysis->traitNeedsDictionary(receiverType->name))
            receiverTrait = receiverType->name;
    }
    const bool receiverIsTrait = !receiverTrait.empty();
    const bool receiverNeedsDictionary = receiverIsTrait &&
        !analysis->traitRequires(receiverTrait, fd.name);
    if (receiverIsTrait && receiverType->kind != KexiType::Constrained)
        receiverType = kexiConstrained(receiverTrait, receiverTrait);
    method.name = fd.name;
    method.receiverType = std::move(receiverType);
    method.isFoul = fd.isFoul;
    method.beamArity = methodBeamArity(fd, firstParamIsReceiver, clause);
    if (receiverNeedsDictionary) ++method.beamArity;
    // When the first clause uses a receiver pattern (@-pattern) or the
    // first param is the receiver (top-level bare function), skip it
    // since receiverType is stored separately.
    bool hasReceiverParam = firstParamIsReceiver ||
        (!fd.clauses.empty() && !fd.clauses[clause].params.empty() && [&]{
        const auto& p0 = fd.clauses[clause].params[0];
        return !p0.name && p0.pattern &&
            (std::holds_alternative<kex::ast::ThisPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<kex::ast::RecordPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<kex::ast::RangePattern>((*p0.pattern)->kind));
    }());
    size_t skipParams = hasReceiverParam ? 1 : 0;

    if (analysis) {
        if (const auto* signatures = analysis->functionSignatures(&fd);
            signatures && !signatures->empty()) {
            const auto& signature = clause < signatures->size()
                ? (*signatures)[clause] : signatures->front();
            // Same rule as collectFromFunctionDef: the type follows
            // inference, the arity follows the annotations the emitter reads.
            for (size_t i = skipParams; i < signature.params.size(); i++)
                method.paramTypes.push_back(
                    convertSemanticType(signature.params[i]));
            method.returnType = convertSemanticType(signature.result);
            method.isFoul = signature.isFoul || fd.isFoul;
        }
    }
    if (analysis && !fd.clauses.empty()) {
        const auto& params = fd.clauses[clause].params;
        for (std::size_t i = skipParams; i < params.size(); ++i) {
            if (!params[i].type || !*params[i].type) continue;
            const auto* name = std::get_if<kex::ast::TypeName>(
                &(*params[i].type)->kind);
            if (!name || name->parts.size() != 1 ||
                !analysis->traitNeedsDictionary(name->parts.front()))
                continue;
            const auto methodIndex = i - skipParams;
            if (methodIndex < method.paramTypes.size())
                method.paramTypes[methodIndex] =
                    kexiConstrained(name->parts.front(), name->parts.front());
            ++method.beamArity;
        }
    }
    if (!method.returnType && !fd.clauses.empty()) {
        auto& params = fd.clauses[clause].params;
        for (size_t i = skipParams; i < params.size(); i++)
            method.paramTypes.push_back(params[i].type && *params[i].type
                ? convertTypeExpr(*params[i].type) : kexiUnknown());
        method.returnType = fd.clauses[clause].returnAnnotation &&
                            *fd.clauses[clause].returnAnnotation
            ? convertTypeExpr(*fd.clauses[clause].returnAnnotation)
            : kexiUnknown();
    }
    if (!fd.clauses.empty()) {
        const auto& params = fd.clauses[clause].params;
        for (size_t i = skipParams; i < params.size(); i++)
            method.paramNames.push_back(params[i].name ? *params[i].name : "");
        method.defaultedParams = trailingDefaults(params, skipParams);
        method.defaultSources = defaultSourcesOf(params, skipParams);
    }
    method.beamFunction = receiverIsTrait && !firstParamIsReceiver
        ? fd.name + "/" + receiverTrait
        : fd.name;
    return method;
}

void addReceiverFunction(const kex::ast::FunctionDef& fd,
                         KexiTypePtr receiverType,
                         KexiTypeInterface& iface,
                         KexiStructuralMetadata& meta,
                         const kex::semantic::Analyzer* analysis,
                         bool firstParamIsReceiver = false,
                         size_t clause = 0) {
    auto method = receiverFunctionFromDef(fd, std::move(receiverType), analysis,
                                          firstParamIsReceiver, clause);
    method.moduleFunction = firstParamIsReceiver;
    meta.methodOwnership.push_back({method.name, method.beamFunction});
    iface.methods.push_back(std::move(method));
}

// Whether `name` is a type declared by `modulePath` itself, and so imported
// under `modulePath.name`. An ADT answers exactly, by its qualified identity:
// `make Weekday` inside `module Time` extends the file's top-level Weekday,
// which is not `Time.Weekday`. A constructor-less type (an opaque type or an
// alias) is known only by its spelling in the interface.
auto ownedByModule(const std::string& name, const std::string& modulePath,
                   const KexiTypeInterface& iface,
                   const KexiStructuralMetadata& meta) -> bool {
    const auto qualified = modulePath + "." + name;
    if (std::any_of(meta.adts.begin(), meta.adts.end(),
                    [&](const auto& adt) { return adt.name == qualified; }))
        return true;
    return std::any_of(iface.types.begin(), iface.types.end(),
                       [&](const auto& type) {
                           return type.name == name && type.constructors.empty();
                       });
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

// Whether a type is incomplete anywhere inside it, not just at the top.
// `[?]` is as unknown a contract as `?` is: an inferred `takeWhile` result of
// `[?]` beat the declared `-> [X]` under a top-level-only test, so callers saw
// `[?]`, and `list.takeWhile(...).at(0)` then matched String's `at` and came
// back as `Char?`.
auto mentionsUnknown(const KexiTypePtr& type) -> bool {
    if (!type) return true;
    if (type->kind == KexiType::Unknown) return true;
    for (const auto& arg : type->typeArgs)
        if (mentionsUnknown(arg)) return true;
    return type->result ? mentionsUnknown(type->result) : false;
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
        if (mentionsUnknown(method.returnType) &&
            !mentionsUnknown(sigType->result))
            method.returnType = sigType->result;
        for (size_t i = 0; i < sigParams; i++)
            if (mentionsUnknown(method.paramTypes[i]) &&
                !mentionsUnknown(sigType->typeArgs[i]))
                method.paramTypes[i] = sigType->typeArgs[i];
    } else {
        if (!method.paramTypes.empty()) return;
        if (mentionsUnknown(method.returnType) && !mentionsUnknown(sigType))
            method.returnType = sigType;
    }
}

// Same as patchMethodWithSig but for module-level / top-level KexiExports.
// Standalone sigs at module scope use the full function type
// (`String -> Result<...>`, params and return together). Skips when the
// sig's arity disagrees with the def's actual arity.
void patchExportWithSig(KexiExport& exp, const KexiTypePtr& sigType,
                        bool authoritative = false) {
    if (!sigType) return;
    if (sigType->kind == KexiType::Func) {
        if (sigType->typeArgs.size() != exp.paramTypes.size()) return;
        if (authoritative) {
            exp.paramTypes = sigType->typeArgs;
            exp.returnType = sigType->result;
            return;
        }
        if (mentionsUnknown(exp.returnType) && !mentionsUnknown(sigType->result))
            exp.returnType = sigType->result;
        for (size_t i = 0; i < exp.paramTypes.size(); i++)
            if (mentionsUnknown(exp.paramTypes[i]) &&
                !mentionsUnknown(sigType->typeArgs[i]))
                exp.paramTypes[i] = sigType->typeArgs[i];
    } else {
        if (!exp.paramTypes.empty()) return;
        if (mentionsUnknown(exp.returnType) && !mentionsUnknown(sigType))
            exp.returnType = sigType;
    }
}

auto hasInlineTraitParam(const kex::ast::FunctionDef& fd,
                         const kex::semantic::Analyzer* analysis,
                         size_t clause = 0) -> bool {
    if (!analysis || fd.clauses.empty()) return false;
    return std::any_of(
        fd.clauses[clause].params.begin(), fd.clauses[clause].params.end(),
        [&](const auto& param) {
            if (!param.type || !*param.type) return false;
            const auto* name =
                std::get_if<kex::ast::TypeName>(&(*param.type)->kind);
            return name && name->parts.size() == 1 &&
                analysis->traitNeedsDictionary(name->parts.front());
        });
}

void applyInlineTraitConstraints(KexiExport& exp,
                                 const kex::ast::FunctionDef& fd,
                                 const kex::semantic::Analyzer* analysis,
                                 size_t clause = 0) {
    if (!analysis || fd.clauses.empty()) return;
    const auto& params = fd.clauses[clause].params;
    for (std::size_t i = 0; i < params.size() && i < exp.paramTypes.size(); ++i) {
        if (!params[i].type || !*params[i].type) continue;
        const auto* trait =
            std::get_if<kex::ast::TypeName>(&(*params[i].type)->kind);
        if (!trait || trait->parts.size() != 1 ||
            !analysis->traitNeedsDictionary(trait->parts.front()))
            continue;
        auto variable = exp.paramTypes[i] &&
                exp.paramTypes[i]->kind == KexiType::Named
            ? exp.paramTypes[i]->name
            : trait->parts.front();
        exp.paramTypes[i] =
            kexiConstrained(std::move(variable), trait->parts.front());
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
                        const kex::semantic::Analyzer* analysis,
                        const std::string& modulePath = {}) {
    auto receiverType = convertTypeExpr(md.target);
    auto typeName = makeTargetName(receiverType);
    // Every type declared inside a module — record, opaque type or ADT — is
    // imported under its qualified identity, and the ADT's constructors are
    // owned by that same qualified name, so the receiver follows it. A type
    // declared elsewhere (`make Weekday` extending a top-level ADT) keeps
    // the spelling it was declared with.
    bool owned = false;
    if (!modulePath.empty() && receiverType &&
        receiverType->kind == KexiType::Named &&
        receiverType->name.find('.') == std::string::npos) {
        owned = ownedByModule(receiverType->name, modulePath, iface, meta);
        if (owned)
            receiverType->name = modulePath + "." + receiverType->name;
    }
    for (const auto& trait : md.implements) {
        if (typeName.empty()) continue;
        const auto traitName = exportedTraitName(trait, modulePath);
        meta.traitConformances.push_back(
            {owned ? modulePath + "." + typeName : typeName, traitName});
        // A make block for a type declared elsewhere may still be naming one
        // of this module's records by a spelling the lookup above missed;
        // the qualified conformance covers it (kexhq/kex#143: `make Files,
        // implement: FS.File` inside `module Mock`).
        if (!owned && !modulePath.empty())
            meta.traitConformances.push_back(
                {modulePath + "." + typeName, traitName});
    }
    // Collect standalone type annotations (`:>` sigs) to patch missing
    // types and to surface pure signatures with no implementation.
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : md.body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    auto addWithSigPatch = [&](const kex::ast::FunctionDef& fd, size_t clause) {
        addReceiverFunction(fd, receiverType, iface, meta, analysis, false,
                            clause);
        iface.methods.back().isSlot = md.isServing && fd.isSlot;
        auto it = standaloneSigs.find(fd.name);
        if (it == standaloneSigs.end()) return;
        auto method = std::move(iface.methods.back());
        iface.methods.pop_back();
        size_t defParams = method.paramTypes.size();
        bool added = false;
        for (const auto& sig : it->second) {
            if (!sig) continue;
            size_t sigParams = (sig->kind == KexiType::Func) ? sig->typeArgs.size() : 0;
            if (sigParams == defParams) {
                auto overload = method;
                patchMethodWithSig(overload, sig);
                iface.methods.push_back(std::move(overload));
                added = true;
            }
        }
        if (!added) iface.methods.push_back(std::move(method));
    };

    std::unordered_set<std::string> seenImpls;
    for (const auto& item : md.body) {
        if (auto* fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fd && *fd) {
            seenImpls.insert((*fd)->name);
            for (const auto* declared : kex::ast::signatureClauses(**fd))
                addWithSigPatch(**fd, static_cast<size_t>(
                    declared - (*fd)->clauses.data()));
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&item);
                   visibility && *visibility && (*visibility)->isPublic) {
            for (const auto& visible : (*visibility)->items)
                if (auto* fd =
                        std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&visible);
                    fd && *fd) {
                    seenImpls.insert((*fd)->name);
                    for (const auto* declared : kex::ast::signatureClauses(**fd))
                        addWithSigPatch(**fd, static_cast<size_t>(
                            declared - (*fd)->clauses.data()));
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
                         const kex::semantic::Analyzer* analysis,
                         const std::string& modulePath = {}) {
    // Exported by its identity, `Geo.Shape` inside `module Geo`.
    const auto identity = kex::traitIdentity(modulePath, trait.name);
    KexiTraitDef traitMetadata;
    traitMetadata.name = identity;
    for (const auto& item : trait.body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann)
            traitMetadata.requiredMethods.push_back(
                {(*ann)->name, (*ann)->isFoul});
    meta.traitDefs.push_back(std::move(traitMetadata));

    auto receiverType = kexiConstrained("T", identity);
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
            for (const auto* declared : kex::ast::signatureClauses(**fd)) {
            const auto clause =
                static_cast<size_t>(declared - (*fd)->clauses.data());
            // Trait defaults are spliced into every concrete implementor by
            // lowering; callers enter through the ordinary runtime dispatcher,
            // not a nonexistent `method/Trait` function.
            addReceiverFunction(**fd, receiverType, iface, meta, analysis,
                                false, clause);
            iface.methods.back().beamFunction = (*fd)->name;
            iface.methods.back().beamArity = 1 +
                static_cast<int>(declared->params.size());
            meta.methodOwnership.back().beamFunction = (*fd)->name;
            auto it = standaloneSigs.find((*fd)->name);
            if (it != standaloneSigs.end()) {
                auto method = std::move(iface.methods.back());
                iface.methods.pop_back();
                for (const auto& sig : it->second) {
                    auto overload = method;
                    patchMethodWithSig(overload, sig);
                    iface.methods.push_back(std::move(overload));
                }
            }
            }
        }
    // Add pure signatures (methods declared but not defaulted in this
    // trait) as type-only methods — no methodOwnership entry.
    for (const auto& [name, sigs] : standaloneSigs)
        if (!seenImpls.count(name))
            for (const auto& sigType : sigs)
                iface.methods.push_back(methodFromSig(name, receiverType, sigType));
}

void collectFromTypeDef(const kex::ast::TypeDef& td, KexiTypeInterface& iface,
                        KexiStructuralMetadata& meta,
                        const std::string& owner = std::string{}) {
    KexiTypeExport te;
    te.name = td.name;
    te.genericParams = td.typeParams;
    te.isDistinct = td.isDistinct;
    // Transparent aliases carry their body too: without it an importing
    // module saw `FS.FilePath` as an opaque nominal name and a `{String:
    // String}` literal failed to match a `Map<FS.FilePath, String>` field.
    // So do structural ones (`type Plug = WebRequest -> WebRequest`).
    if (td.variants && !td.variants->empty() &&
        (td.isDistinct || kex::isTransparentTypeAlias(td)))
        te.backingType = convertTypeExpr(td.variants->front());
    else if (const auto* target = kex::aliasTarget(td))
        te.backingType = convertTypeExprImpl(*target);
    // A union of atom literals (`type FileKind = :file | :directory`) is an
    // alias too, and without its body an importer saw an opaque `FileKind`
    // that no `:file` could satisfy.
    if (!te.backingType && td.variants && !td.variants->empty() &&
        !td.isDistinct && td.typeParams.empty() &&
        std::all_of(td.variants->begin(), td.variants->end(), [](const auto& v) {
            return v && std::holds_alternative<kex::ast::AtomType>(v->kind);
        })) {
        std::vector<KexiTypePtr> members;
        for (const auto& v : *td.variants) members.push_back(convertTypeExpr(v));
        te.backingType = kexiUnion(std::move(members));
    }
    // A transparent alias (`type FilePath = String`) has no constructors —
    // recording `String` as one made every bare `String` widen to `FilePath`.
    if (td.variants && !td.isDistinct && !kex::isTransparentTypeAlias(td)) {
        // An ADT declared inside a module is known by its qualified name, as
        // a record is: `Kex.Backend`, never a bare `Backend` that a same-named
        // constructor elsewhere (NetErrorKind's) could be mistaken for.
        KexiADT adt;
        adt.name = owner.empty() ? td.name : owner + "." + td.name;
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
                    for (const auto& arg : gt->args) {
                        int slot = -1;
                        const auto* named = arg
                            ? std::get_if<kex::ast::TypeName>(&arg->kind)
                            : nullptr;
                        std::string variable;
                        if (named && named->parts.size() == 1)
                            variable = named->parts.front();
                        else if (const auto* generic = arg
                                     ? std::get_if<kex::ast::GenericVar>(&arg->kind)
                                     : nullptr)
                            variable = generic->name;
                        if (!variable.empty())
                            if (auto parameter = std::find(td.typeParams.begin(),
                                                           td.typeParams.end(),
                                                           variable);
                                parameter != td.typeParams.end())
                                slot = static_cast<int>(parameter - td.typeParams.begin());
                        ctor.typeParamSlots.push_back(slot);
                    }
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

// `owner` is the module path the record is declared in, or empty for a
// file's top level. It decides the tuple tag: lowering qualifies a record
// declared inside a `module` block and leaves a top-level one bare, and the
// metadata has to agree or nothing can match the runtime value.
void collectFromRecordDef(const kex::ast::RecordDef& rd, KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta,
                          const std::string& owner = std::string{}) {
    KexiTypeExport te;
    te.name = rd.name;
    te.genericParams = rd.typeParams;
    iface.types.push_back(std::move(te));

    KexiRecord rec;
    rec.name = rd.name;
    rec.tagAtom = owner.empty() ? rd.name : owner + "." + rd.name;
    for (const auto& f : rd.fields) {
        KexiRecordField rf;
        rf.name = f.name;
        rf.type = convertTypeExpr(f.type);
        rf.defaultSource = defaultSourceOf(f.defaultValue);
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
                           const kex::semantic::Analyzer* analysis,
                           const std::string& modulePath = std::string{}) {
    std::unordered_map<std::string, std::vector<KexiTypePtr>> standaloneSigs;
    for (const auto& item : body)
        if (auto* ann = std::get_if<std::unique_ptr<kex::ast::TypeAnnotation>>(&item);
            ann && *ann && (*ann)->type)
            standaloneSigs[(*ann)->name].push_back(convertTypeExpr((*ann)->type));

    std::unordered_set<std::string> seenImpls;
    auto addFunction = [&](const kex::ast::FunctionDef& fd, size_t clause) {
        seenImpls.insert(fd.name);
        collectFromFunctionDef(fd, iface, analysis, clause);
        meta.publicExports.push_back(fd.name);
        auto it = standaloneSigs.find(fd.name);
        if (it != standaloneSigs.end()) {
            auto exported = std::move(iface.exports.back());
            iface.exports.pop_back();
            for (const auto& sig : it->second) {
                auto overload = exported;
                patchExportWithSig(overload, sig,
                                   hasInlineTraitParam(fd, analysis, clause));
                applyInlineTraitConstraints(overload, fd, analysis, clause);
                iface.exports.push_back(std::move(overload));
            }
        }
        // A nested companion module's typed operations participate in UFCS
        // when its parent is imported. Keep plain namespace modules
        // qualified-only so they cannot pollute global receiver selection.
        if (modulePath.find('.') != std::string::npos &&
            !fd.clauses.empty() && !fd.clauses[clause].params.empty()) {
            const auto& first = fd.clauses[clause].params.front();
            auto receiver = first.type && *first.type
                ? convertTypeExpr(*first.type) : kexiUnknown();
            if (receiver && receiver->kind == KexiType::Named &&
                receiver->name.find('.') == std::string::npos) {
                if (ownedByModule(receiver->name, modulePath, iface, meta))
                    receiver->name = modulePath + "." + receiver->name;
            }
            addReceiverFunction(fd, std::move(receiver), iface, meta,
                                analysis, true, clause);
            if (it != standaloneSigs.end()) {
                auto method = std::move(iface.methods.back());
                iface.methods.pop_back();
                bool added = false;
                for (const auto& sig : it->second) {
                    if (!sig || sig->kind != KexiType::Func ||
                        sig->typeArgs.size() !=
                            fd.clauses[clause].params.size())
                        continue;
                    // The signature names the receiver too (`get : String
                    // -> Plug -> Entry`): it types the receiver, which was
                    // left unknown — matching every receiver — when the
                    // definition wrote no type of its own, and the rest
                    // types the parameters after it.
                    auto overload = method;
                    if (mentionsUnknown(overload.receiverType) &&
                        !sig->typeArgs.empty() &&
                        !mentionsUnknown(sig->typeArgs.front())) {
                        overload.receiverType =
                            std::make_shared<KexiType>(*sig->typeArgs.front());
                        auto& receiver = overload.receiverType;
                        if (receiver->kind == KexiType::Named &&
                            receiver->name.find('.') == std::string::npos &&
                            ownedByModule(receiver->name, modulePath, iface, meta))
                            receiver->name = modulePath + "." + receiver->name;
                    }
                    auto rest = std::make_shared<KexiType>(*sig);
                    if (!rest->typeArgs.empty())
                        rest->typeArgs.erase(rest->typeArgs.begin());
                    patchMethodWithSig(overload, rest);
                    iface.methods.push_back(std::move(overload));
                    added = true;
                }
                if (!added) iface.methods.push_back(std::move(method));
            }
        }
    };

    for (const auto& item : body) {
        std::visit([&](const auto& ptr) {
            if (!ptr) return;
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                for (const auto* declared : kex::ast::signatureClauses(*ptr))
                    addFunction(*ptr, static_cast<size_t>(
                        declared - ptr->clauses.data()));
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, analysis, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::TraitDef>) {
                collectFromTraitDef(*ptr, iface, meta, analysis, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta, modulePath);
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
    auto addFunction = [&](const kex::ast::FunctionDef& fd, size_t clause) {
        seenImpls.insert(fd.name);
        collectFromFunctionDef(fd, iface, analysis, clause);
        meta.publicExports.push_back(fd.name);
        auto it = standaloneSigs.find(fd.name);
        if (it != standaloneSigs.end()) {
            auto exported = std::move(iface.exports.back());
            iface.exports.pop_back();
            for (const auto& sig : it->second) {
                auto overload = exported;
                patchExportWithSig(overload, sig,
                                   hasInlineTraitParam(fd, analysis, clause));
                applyInlineTraitConstraints(overload, fd, analysis, clause);
                iface.exports.push_back(std::move(overload));
            }
        }
        // A top-level prelude function is also reachable through UFCS, with its
        // first parameter as the receiver. One parameter is enough — that is
        // the no-argument postfix form (`x.inspect`), which was previously
        // excluded by requiring two and so failed with "Undefined method" on
        // BEAM while working in the interpreter.
        if (!fd.clauses.empty() && fd.clauses[clause].params.size() >= 1) {
            KexiTypePtr receiver = kexiUnknown();
            const auto& first = fd.clauses[clause].params[0];
            if (first.type && *first.type) {
                receiver = convertTypeExpr(*first.type);
                if (analysis) {
                    if (const auto* name = std::get_if<kex::ast::TypeName>(
                            &(*first.type)->kind);
                        name && name->parts.size() == 1 &&
                        analysis->traitNeedsDictionary(name->parts.front()))
                        receiver = kexiConstrained(name->parts.front(),
                                                   name->parts.front());
                }
            }
            addReceiverFunction(fd, std::move(receiver), iface, meta,
                                analysis, true, clause);
        }
    };

    for (const auto& item : program.items) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::FunctionDef>) {
                for (const auto* declared : kex::ast::signatureClauses(*ptr))
                    addFunction(*ptr, static_cast<size_t>(
                        declared - ptr->clauses.data()));
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
                for (const auto* declared : kex::ast::signatureClauses(*ptr)) {
                const auto clause =
                    static_cast<size_t>(declared - ptr->clauses.data());
                collectFromFunctionDef(*ptr, iface, analysis, clause);
                auto& exp = iface.exports.back();
                exp.name = modulePath + "." + ptr->name;
                exp.beamFunction = emittedPrefix + "__" + ptr->name;
                meta.publicExports.push_back(exp.name);
                auto it = standaloneSigs.find(ptr->name);
                if (it != standaloneSigs.end())
                    for (const auto& sig : it->second)
                        patchExportWithSig(exp, sig,
                                           hasInlineTraitParam(*ptr, analysis, clause));
                applyInlineTraitConstraints(exp, *ptr, analysis, clause);
                }
            } else if constexpr (std::is_same_v<T, kex::ast::MakeDef>) {
                collectFromMakeDef(*ptr, iface, meta, analysis, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::TraitDef>) {
                collectFromTraitDef(*ptr, iface, meta, analysis, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::TypeDef>) {
                collectFromTypeDef(*ptr, iface, meta, modulePath);
            } else if constexpr (std::is_same_v<T, kex::ast::RecordDef>) {
                collectFromRecordDef(*ptr, iface, meta, modulePath);
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
    // Every part: a module can be declared more than once (`capability
    // Metro.Storage` beside `module Metro.Storage`, or `module Float` in two
    // files), and stopping at the first left the rest out of its interface.
    bool found = false;
    for (const auto& item : body)
        if (auto* module =
                std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
            if ((*module)->name == moduleName) {
                collectFromModuleBody((*module)->body, iface, meta, analysis,
                                      moduleName);
                found = true;
            } else if (findAndCollectModuleBody((*module)->body, moduleName,
                                                iface, meta, analysis)) {
                found = true;
            }
        }
    return found;
}

// Find a top-level or nested ModuleDef by its qualified source name.
auto findAndCollectModule(const kex::ast::Program& program,
                          const std::string& moduleName,
                          KexiTypeInterface& iface,
                          KexiStructuralMetadata& meta,
                          const kex::semantic::Analyzer* analysis) -> bool {
    bool found = false;
    for (const auto& item : program.items)
        if (auto* module =
                std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
            if ((*module)->name == moduleName) {
                collectFromModuleBody((*module)->body, iface, meta, analysis,
                                      moduleName);
                found = true;
            } else if (findAndCollectModuleBody((*module)->body, moduleName,
                                                iface, meta, analysis)) {
                found = true;
            }
        }
    return found;
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

    TraitExportScope traitScope;
    kex::forEachTraitDef(program.items, [&](const ast::TraitDef& trait,
                                            const std::string& modulePath) {
        traitScope.identities.insert(kex::traitIdentity(modulePath, trait.name));
    });
    kex::collectScopeUsings(program.items, traitScope.usings);
    tTraitExportScope = &traitScope;
    struct ResetScope {
        ~ResetScope() { tTraitExportScope = nullptr; }
    } resetScope;

    // Whether this module was declared `capability`. A consumer reads its
    // stdlib interfaces from this chunk rather than from source, so the flag
    // has to travel with it or a stdlib capability cannot be replaced by any
    // importer (kexhq/kex#143).
    if (!opts.moduleName.empty()) {
        std::function<bool(const ast::ModuleDef&)> visit =
            [&](const ast::ModuleDef& mod) -> bool {
            if (mod.name == opts.moduleName) {
                chunk.metadata.isCapability = mod.isCapability;
                return true;
            }
            for (const auto& item : mod.body)
                if (const auto* nested =
                        std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested && visit(**nested)) return true;
            return false;
        };
        for (const auto& item : program.items)
            if (const auto* mod =
                    std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                if (*mod && visit(**mod)) break;
        // Every part declared `capability` names its members.
        std::function<void(const ast::ModuleDef&)> members =
            [&](const ast::ModuleDef& mod) {
            if (mod.name == opts.moduleName && mod.isCapability)
                for (const auto& item : mod.body)
                    if (const auto* function =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                        function && *function &&
                        std::find(chunk.metadata.capabilityMembers.begin(),
                                  chunk.metadata.capabilityMembers.end(),
                                  (*function)->name) ==
                            chunk.metadata.capabilityMembers.end())
                        chunk.metadata.capabilityMembers.push_back((*function)->name);
            for (const auto& item : mod.body)
                if (const auto* nested =
                        std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) members(**nested);
        };
        if (opts.unitCompiledApart)
            for (const auto& item : program.items)
                if (const auto* mod =
                        std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*mod) members(**mod);
    }

    if (!opts.noCheck) {
        if (opts.flattenModules)
            collectFlattenedProgram(program, chunk.typeInterface, chunk.metadata,
                                    opts.analysis);
        else if (!opts.collectTopLevel && !opts.moduleName.empty()) {
            findAndCollectModule(program, opts.moduleName,
                                 chunk.typeInterface, chunk.metadata,
                                 opts.analysis);
            // A `make` block written outside the module for one of its types
            // (`make Type.Field` after `module Type`) is the module's too.
            if (opts.unitCompiledApart) {
                for (const auto& item : program.items)
                    if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
                        make && *make &&
                        makeTargetName(convertTypeExpr((*make)->target))
                                .rfind(opts.moduleName + ".", 0) == 0)
                        collectFromMakeDef(**make, chunk.typeInterface, chunk.metadata,
                                           opts.analysis);
            }
        }
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

namespace {
auto qualifyType(const KexiTypePtr& type,
                 const std::function<std::string(const std::string&)>& qualify) -> void {
    if (!type) return;
    if (type->kind == KexiType::Named && type->name.find('.') == std::string::npos)
        type->name = qualify(type->name);
    for (const auto& arg : type->typeArgs) qualifyType(arg, qualify);
    qualifyType(type->result, qualify);
}
} // namespace

auto qualifyTypeNames(KexiChunk& chunk,
                      const std::function<std::string(const std::string&)>& qualify)
    -> void {
    auto& iface = chunk.typeInterface;
    for (auto& exported : iface.exports) {
        for (auto& param : exported.paramTypes) qualifyType(param, qualify);
        qualifyType(exported.returnType, qualify);
    }
    for (auto& constant : iface.constants) qualifyType(constant.type, qualify);
    for (auto& type : iface.types) qualifyType(type.backingType, qualify);
    for (auto& method : iface.methods) {
        qualifyType(method.receiverType, qualify);
        for (auto& param : method.paramTypes) qualifyType(param, qualify);
        qualifyType(method.returnType, qualify);
    }
    for (auto& record : chunk.metadata.records)
        for (auto& field : record.fields) qualifyType(field.type, qualify);
}

} // namespace kex::beam

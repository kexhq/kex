#include "interface_cache.hxx"

#include "etf.hxx"

#include "../common/timings.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "../semantic/traits.hxx"
#include "../semantic/types.hxx"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::beam {

namespace {

using kex::semantic::Type;
using kex::semantic::TypePtr;
using namespace kex::semantic;

constexpr int kMagic = 0x4b584946;      // "KXIF"
// Bump with ANY shape change below: old files must read as bad-version,
// never misparse.
constexpr int kVersion = 5;

// Every tuple shape in the format, named once and shared by the encoder
// and the decoder. Named shapes (ifaces, fn, signature, ...) go through
// checkedTuple on write and an explicit size check on read; type-tag
// shapes pin their size on read only (a tag is a closed vocabulary entry,
// not an extension point); anonymous key/value pairs rely on the
// decoder's exception path (short input throws EtfError → corrupt).
// When you add a field: extend the shape on BOTH sides, bump kVersion,
// and extend the round-trip agreement test — the test is what catches a
// field the decoder silently drops.
namespace schema {
constexpr size_t kIfaces = 14;
constexpr size_t kModulePair = 2;
constexpr size_t kModuleBody = 6;
constexpr size_t kSignature = 9;
constexpr size_t kFn = 17;
constexpr size_t kTrait = 5;
constexpr size_t kAdt = 9;
constexpr size_t kDistinct = 3;
} // namespace schema

auto checkedTuple(std::vector<TermPtr> elems, size_t expected, const char* what)
    -> TermPtr {
    if (elems.size() != expected)
        throw EtfError(std::string("encode: ") + what + " shape");
    return Term::tuple(std::move(elems));
}

// ---- Type <-> Term ---------------------------------------------------------

// Adding a Type alternative without extending the codec below would
// silently degrade to "unknown" — fail the build instead.
static_assert(std::variant_size_v<decltype(Type::kind)> == 16,
              "Type gained an alternative: extend typeToTerm/typeFromSax "
              "and the round-trip agreement test");

auto typeToTerm(const TypePtr& t) -> TermPtr;

auto listOf(const std::vector<TypePtr>& types) -> TermPtr {
    std::vector<TermPtr> elems;
    elems.reserve(types.size());
    for (const auto& t : types) elems.push_back(typeToTerm(t));
    return Term::list(std::move(elems));
}

// Every Type alternative's wire tag and tuple size, in one table the encoder
// and the decoder both read: a tag spelled differently on the two sides, or a
// size changed on one, cannot happen. `unknown` and `null` travel as bare
// atoms (see typeToTerm) and have no row.
enum class TypeTag {
    Primitive, SizedInt, SizedFloat, Named, Func, Tuple, List, Map, Optional,
    Union, Intersection, Record, TypeVar, Void, Constrained,
};

struct TypeShape {
    TypeTag tag;
    std::string_view atom;
    size_t arity;  // tuple size, the tag included
};

constexpr TypeShape kTypeShapes[] = {
    {TypeTag::Primitive, "primitive", 3},
    {TypeTag::SizedInt, "sized_int", 3},
    {TypeTag::SizedFloat, "sized_float", 2},
    {TypeTag::Named, "named", 4},
    {TypeTag::Func, "func", 4},
    {TypeTag::Tuple, "tuple", 2},
    {TypeTag::List, "list", 2},
    {TypeTag::Map, "map", 3},
    {TypeTag::Optional, "optional", 2},
    {TypeTag::Union, "union", 2},
    {TypeTag::Intersection, "intersection", 2},
    {TypeTag::Record, "record", 2},
    {TypeTag::TypeVar, "typevar", 2},
    {TypeTag::Void, "void", 2},
    {TypeTag::Constrained, "constrained", 4},
};

auto shapeOf(TypeTag tag) -> const TypeShape& {
    for (const auto& shape : kTypeShapes)
        if (shape.tag == tag) return shape;
    throw EtfError("encode: type tag without a shape");
}

auto shapeOf(std::string_view atom) -> const TypeShape& {
    for (const auto& shape : kTypeShapes)
        if (shape.atom == atom) return shape;
    throw EtfError("type: unknown tag " + std::string(atom));
}

// `{Tag, Fields...}`, its size checked against the table.
auto typeTuple(TypeTag tag, std::vector<TermPtr> fields) -> TermPtr {
    const auto& shape = shapeOf(tag);
    fields.insert(fields.begin(), Term::atom(std::string(shape.atom)));
    return checkedTuple(std::move(fields), shape.arity, shape.atom.data());
}

// A primitive kind goes on the wire by the name Kex writes it with.
auto primitiveKindAtom(PrimitiveType::Kind kind) -> std::string {
    return std::string(kex::semantic::primitiveKindName(kind));
}

auto primitiveKindOf(std::string_view name) -> PrimitiveType::Kind {
    if (const auto kind = kex::semantic::primitiveKindNamed(name)) return *kind;
    throw EtfError("primitive: bad kind");
}

auto typeToTerm(const TypePtr& t) -> TermPtr {
    // A null type is NOT UnknownType: it round-trips as "null" so a cache
    // hit observes exactly what a cache miss derived. ("unknown" stays the
    // UnknownType tag, so old files decode unchanged.)
    if (!t) return Term::atom("null");
    return std::visit([&](const auto& node) -> TermPtr {
        using K = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<K, PrimitiveType>) {
            return typeTuple(TypeTag::Primitive,
                             {Term::atom(primitiveKindAtom(node.kind)),
                              Term::binary(node.atomName)});
        } else if constexpr (std::is_same_v<K, SizedIntType>) {
            return typeTuple(TypeTag::SizedInt, {Term::integer(node.bits),
                                                 Term::boolean(node.isSigned)});
        } else if constexpr (std::is_same_v<K, SizedFloatType>) {
            return typeTuple(TypeTag::SizedFloat, {Term::integer(node.bits)});
        } else if constexpr (std::is_same_v<K, NamedType>) {
            return typeTuple(TypeTag::Named, {Term::binary(node.name), listOf(node.typeArgs),
                                              Term::binary(node.variant)});
        } else if constexpr (std::is_same_v<K, FuncType>) {
            return typeTuple(TypeTag::Func,
                             {listOf(node.params), typeToTerm(node.result),
                              Term::boolean(node.block)});
        } else if constexpr (std::is_same_v<K, TupleType>) {
            return typeTuple(TypeTag::Tuple, {listOf(node.elements)});
        } else if constexpr (std::is_same_v<K, ListType>) {
            return typeTuple(TypeTag::List, {typeToTerm(node.element)});
        } else if constexpr (std::is_same_v<K, MapType>) {
            return typeTuple(TypeTag::Map,
                             {typeToTerm(node.key), typeToTerm(node.value)});
        } else if constexpr (std::is_same_v<K, OptionalType>) {
            return typeTuple(TypeTag::Optional, {typeToTerm(node.inner)});
        } else if constexpr (std::is_same_v<K, UnionType>) {
            return typeTuple(TypeTag::Union, {listOf(node.members)});
        } else if constexpr (std::is_same_v<K, IntersectionType>) {
            return typeTuple(TypeTag::Intersection, {listOf(node.members)});
        } else if constexpr (std::is_same_v<K, RecordType>) {
            std::vector<TermPtr> fields;
            for (const auto& [name, fieldType] : node.fields)
                fields.push_back(Term::tuple({Term::binary(name),
                                              typeToTerm(fieldType)}));
            return typeTuple(TypeTag::Record, {Term::list(std::move(fields))});
        } else if constexpr (std::is_same_v<K, TypeVar>) {
            return typeTuple(TypeTag::TypeVar, {Term::integer(node.id)});
        } else if constexpr (std::is_same_v<K, UnknownType>) {
            return Term::atom("unknown");
        } else if constexpr (std::is_same_v<K, VoidType>) {
            return typeTuple(TypeTag::Void, {Term::binary(node.reason)});
        } else if constexpr (std::is_same_v<K, ConstrainedType>) {
            return typeTuple(TypeTag::Constrained,
                             {Term::binary(node.varName),
                              Term::binary(node.traitName),
                              Term::integer(node.genericId)});
        }
        return Term::atom("unknown");
    }, t->kind);
}

// SAX twin of the deleted tree walk: same wire, same checks, same errors,
// straight into the structs (no tree, no second pass).
// Throws EtfError on any malformed input.
auto typeFromSax(SaxReader& sax) -> TypePtr {
    if (!sax.nextIsTuple()) {
        if (!sax.nextIsAtom()) throw EtfError("type: expected tuple");
        const auto bare = sax.readAtom();
        if (bare == "null") return nullptr;
        if (bare == "unknown")
            return std::make_shared<Type>(Type{UnknownType{}});
        throw EtfError("type: expected tuple");
    }
    const auto arity = sax.beginTuple();
    if (arity == 0) throw EtfError("type: empty tuple");
    const auto& shape = shapeOf(sax.readAtom());
    if (arity != shape.arity)
        throw EtfError(std::string(shape.atom) + ": arity");
    auto readTypes = [&](std::vector<TypePtr>& out) {
        const auto count = sax.beginList();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i)
            out.push_back(typeFromSax(sax));
        sax.endList();
    };
    auto make = [](auto kind) { return std::make_shared<Type>(Type{std::move(kind)}); };
    switch (shape.tag) {
    case TypeTag::Primitive: {
        const auto kind = primitiveKindOf(sax.readAtom());
        return make(PrimitiveType{kind, sax.readBinary()});
    }
    case TypeTag::SizedInt: {
        const auto bits = static_cast<int>(sax.readInt());
        return make(SizedIntType{bits, sax.readBool()});
    }
    case TypeTag::SizedFloat:
        return make(SizedFloatType{static_cast<int>(sax.readInt())});
    case TypeTag::Named: {
        NamedType named{sax.readBinary(), {}};
        readTypes(named.typeArgs);
        named.variant = sax.readBinary();
        return make(std::move(named));
    }
    case TypeTag::Func: {
        FuncType fn;
        readTypes(fn.params);
        fn.result = typeFromSax(sax);
        fn.block = sax.readBool();
        return make(std::move(fn));
    }
    case TypeTag::Tuple: {
        TupleType tuple;
        readTypes(tuple.elements);
        return make(std::move(tuple));
    }
    case TypeTag::List:
        return make(ListType{typeFromSax(sax)});
    case TypeTag::Map: {
        auto key = typeFromSax(sax);
        auto value = typeFromSax(sax);
        return make(MapType{std::move(key), std::move(value)});
    }
    case TypeTag::Optional:
        return make(OptionalType{typeFromSax(sax)});
    case TypeTag::Union: {
        UnionType unionType;
        readTypes(unionType.members);
        return make(std::move(unionType));
    }
    case TypeTag::Intersection: {
        IntersectionType intersection;
        readTypes(intersection.members);
        return make(std::move(intersection));
    }
    case TypeTag::Record: {
        RecordType record;
        const auto count = sax.beginList();
        record.fields.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            sax.beginTuple();
            auto name = sax.readBinary();
            auto fieldType = typeFromSax(sax);
            record.fields.emplace_back(std::move(name), std::move(fieldType));
        }
        sax.endList();
        return make(std::move(record));
    }
    case TypeTag::TypeVar:
        return make(TypeVar{static_cast<int>(sax.readInt())});
    case TypeTag::Void:
        return make(VoidType{sax.readBinary()});
    case TypeTag::Constrained: {
        auto varName = sax.readBinary();
        auto traitName = sax.readBinary();
        return make(ConstrainedType{std::move(varName), std::move(traitName),
                                    static_cast<int>(sax.readInt())});
    }
    }
    throw EtfError("type: unknown tag " + std::string(shape.atom));
}

// ---- Signature <-> Term ----------------------------------------------------

auto signatureToTerm(const kex::semantic::Signature& sig) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : sig.params) params.push_back(typeToTerm(p));
    // How many parameters a call must pass, or `none` where none was recorded.
    TermPtr required = sig.requiredParams
        ? Term::integer(static_cast<int64_t>(*sig.requiredParams))
        : Term::atom("none");
    std::vector<TermPtr> names;
    for (const auto& n : sig.paramNames) names.push_back(Term::binary(n));
    return checkedTuple({Term::atom("signature"), Term::binary(sig.name),
                         Term::list(std::move(params)), typeToTerm(sig.result),
                         Term::boolean(sig.isFoul), required,
                         Term::list(std::move(names)), Term::binary(sig.makeModule),
                         Term::binary(sig.declaringModule)},
                        schema::kSignature, "signature");
}

auto signatureFromSax(SaxReader& sax) -> kex::semantic::Signature {
    const auto arity = sax.beginTuple();
    if (arity != schema::kSignature) throw EtfError("signature: shape");
    if (sax.readAtom() != "signature") throw EtfError("signature: shape");
    kex::semantic::Signature sig;
    sig.name = sax.readBinary();
    const auto paramCount = sax.beginList();
    sig.params.reserve(paramCount);
    for (size_t i = 0; i < paramCount; ++i)
        sig.params.push_back(typeFromSax(sax));
    sax.endList();
    sig.result = typeFromSax(sax);
    sig.isFoul = sax.readBool();
    if (sax.nextIsAtom())
        sax.readAtom();
    else
        sig.requiredParams = static_cast<size_t>(sax.readInt());
    const auto nameCount = sax.beginList();
    sig.paramNames.reserve(nameCount);
    for (size_t i = 0; i < nameCount; ++i)
        sig.paramNames.push_back(sax.readBinary());
    sax.endList();
    sig.makeModule = sax.readBinary();
    sig.declaringModule = sax.readBinary();
    return sig;
}

// ---- ImportedFunction <-> Term ---------------------------------------------

auto functionToTerm(const kex::semantic::ImportedFunction& fn) -> TermPtr {
    std::vector<TermPtr> names;
    for (const auto& n : fn.paramNames) names.push_back(Term::binary(n));
    std::vector<TermPtr> defaults;
    for (const auto& d : fn.defaultSources) defaults.push_back(Term::binary(d));
    return checkedTuple({Term::atom("fn"), Term::binary(fn.sourceName),
                         Term::binary(fn.backendFunction),
                         Term::binary(fn.backendModule),
                         Term::integer(fn.backendArity),
                         Term::list(std::move(names)),
                         Term::list(std::move(defaults)),
                         signatureToTerm(fn.signature),
                         Term::binary(fn.sourceModule),
                         Term::boolean(fn.isReceiverMethod),
                         Term::boolean(fn.visibleByReceiver),
                         Term::boolean(fn.isConstructor),
                         Term::boolean(fn.remoteSlot),
                         Term::binary(fn.constructorOwner),
                         Term::binary(fn.implementationHead),
                         Term::boolean(fn.foreignTarget),
                         Term::boolean(fn.aliasReceiver)},
                        schema::kFn, "fn");
}

auto functionFromSax(SaxReader& sax) -> kex::semantic::ImportedFunction {
    const auto arity = sax.beginTuple();
    if (arity != schema::kFn) throw EtfError("fn: shape");
    if (sax.readAtom() != "fn") throw EtfError("fn: shape");
    kex::semantic::ImportedFunction fn;
    fn.sourceName = sax.readBinary();
    fn.backendFunction = sax.readBinary();
    fn.backendModule = sax.readBinary();
    fn.backendArity = static_cast<int>(sax.readInt());
    const auto nameCount = sax.beginList();
    fn.paramNames.reserve(nameCount);
    for (size_t i = 0; i < nameCount; ++i)
        fn.paramNames.push_back(sax.readBinary());
    sax.endList();
    const auto defaultCount = sax.beginList();
    fn.defaultSources.reserve(defaultCount);
    for (size_t i = 0; i < defaultCount; ++i)
        fn.defaultSources.push_back(sax.readBinary());
    sax.endList();
    fn.signature = signatureFromSax(sax);
    fn.sourceModule = sax.readBinary();
    fn.isReceiverMethod = sax.readBool();
    fn.visibleByReceiver = sax.readBool();
    fn.isConstructor = sax.readBool();
    fn.remoteSlot = sax.readBool();
    fn.constructorOwner = sax.readBinary();
    fn.implementationHead = sax.readBinary();
    fn.foreignTarget = sax.readBool();
    fn.aliasReceiver = sax.readBool();
    return fn;
}

// ---- Traits <-> Term -------------------------------------------------------

auto traitToTerm(const kex::semantic::TraitDef& trait) -> TermPtr {
    std::vector<TermPtr> required;
    for (const auto& sig : trait.requiredMethods)
        required.push_back(signatureToTerm(sig));
    std::vector<TermPtr> defaults;
    for (const auto& name : trait.defaultMethods)
        defaults.push_back(Term::binary(name));
    std::vector<TermPtr> bodies;
    for (const auto& name : trait.defaultBodies) bodies.push_back(Term::binary(name));
    return checkedTuple({Term::atom("trait"), Term::binary(trait.name),
                         Term::list(std::move(required)),
                         Term::list(std::move(defaults)),
                         Term::list(std::move(bodies))},
                        schema::kTrait, "trait");
}

auto traitFromSax(SaxReader& sax) -> kex::semantic::TraitDef {
    const auto arity = sax.beginTuple();
    if (arity != schema::kTrait) throw EtfError("trait: shape");
    if (sax.readAtom() != "trait") throw EtfError("trait: shape");
    kex::semantic::TraitDef trait;
    trait.name = sax.readBinary();
    const auto requiredCount = sax.beginList();
    trait.requiredMethods.reserve(requiredCount);
    for (size_t i = 0; i < requiredCount; ++i)
        trait.requiredMethods.push_back(signatureFromSax(sax));
    sax.endList();
    const auto defaultCount = sax.beginList();
    trait.defaultMethods.reserve(defaultCount);
    for (size_t i = 0; i < defaultCount; ++i)
        trait.defaultMethods.push_back(sax.readBinary());
    sax.endList();
    const auto bodyCount = sax.beginList();
    for (size_t i = 0; i < bodyCount; ++i)
        trait.defaultBodies.push_back(sax.readBinary());
    sax.endList();
    return trait;
}

// ---- ADT / distinct <-> Term ------------------------------------------------

auto adtToTerm(const kex::semantic::ImportedADT& adt) -> TermPtr {
    std::vector<TermPtr> ctors;
    for (const auto& c : adt.constructors) ctors.push_back(Term::binary(c));
    std::vector<TermPtr> arities;
    for (const auto& [ctor, arity] : adt.constructorArities)
        arities.push_back(Term::tuple({Term::binary(ctor), Term::integer(arity)}));
    std::vector<TermPtr> slots;
    for (const auto& [ctor, indices] : adt.constructorTypeParamSlots) {
        std::vector<TermPtr> idx;
        for (const auto& i : indices) idx.push_back(Term::integer(i));
        slots.push_back(Term::tuple({Term::binary(ctor), Term::list(std::move(idx))}));
    }
    std::vector<TermPtr> paramNames;
    for (const auto& n : adt.typeParamNames) paramNames.push_back(Term::binary(n));
    std::vector<TermPtr> paramTypes;
    for (const auto& [ctor, types] : adt.constructorParamTypes)
        paramTypes.push_back(Term::tuple({Term::binary(ctor), listOf(types)}));
    return checkedTuple({Term::atom("adt"), Term::binary(adt.name), Term::binary(adt.module),
                         Term::list(std::move(ctors)), Term::list(std::move(arities)),
                         Term::integer(static_cast<int64_t>(adt.typeParamCount)),
                         Term::list(std::move(slots)), Term::list(std::move(paramNames)),
                         Term::list(std::move(paramTypes))},
                        schema::kAdt, "adt");
}

auto adtFromSax(SaxReader& sax) -> kex::semantic::ImportedADT {
    const auto arity = sax.beginTuple();
    if (arity != schema::kAdt) throw EtfError("adt: shape");
    if (sax.readAtom() != "adt") throw EtfError("adt: shape");
    kex::semantic::ImportedADT adt;
    adt.name = sax.readBinary();
    adt.module = sax.readBinary();
    const auto ctorCount = sax.beginList();
    adt.constructors.reserve(ctorCount);
    for (size_t i = 0; i < ctorCount; ++i)
        adt.constructors.push_back(sax.readBinary());
    sax.endList();
    const auto arityCount = sax.beginList();
    adt.constructorArities.reserve(arityCount);
    for (size_t i = 0; i < arityCount; ++i) {
        sax.beginTuple();
        auto ctor = sax.readBinary();
        const auto value = static_cast<int>(sax.readInt());
        adt.constructorArities[std::move(ctor)] = value;
    }
    sax.endList();
    adt.typeParamCount = static_cast<size_t>(sax.readInt());
    const auto slotCount = sax.beginList();
    adt.constructorTypeParamSlots.reserve(slotCount);
    for (size_t i = 0; i < slotCount; ++i) {
        sax.beginTuple();
        auto ctor = sax.readBinary();
        std::vector<int> idx;
        const auto idxCount = sax.beginList();
        idx.reserve(idxCount);
        for (size_t j = 0; j < idxCount; ++j)
            idx.push_back(static_cast<int>(sax.readInt()));
        sax.endList();
        adt.constructorTypeParamSlots[std::move(ctor)] = std::move(idx);
    }
    sax.endList();
    const auto paramNameCount = sax.beginList();
    adt.typeParamNames.reserve(paramNameCount);
    for (size_t i = 0; i < paramNameCount; ++i)
        adt.typeParamNames.push_back(sax.readBinary());
    sax.endList();
    const auto paramTypeCount = sax.beginList();
    adt.constructorParamTypes.reserve(paramTypeCount);
    for (size_t i = 0; i < paramTypeCount; ++i) {
        sax.beginTuple();
        auto ctor = sax.readBinary();
        std::vector<TypePtr> types;
        const auto typeCount = sax.beginList();
        types.reserve(typeCount);
        for (size_t j = 0; j < typeCount; ++j)
            types.push_back(typeFromSax(sax));
        sax.endList();
        adt.constructorParamTypes[std::move(ctor)] = std::move(types);
    }
    sax.endList();
    return adt;
}

auto distinctToTerm(const kex::semantic::ImportedDistinctType& distinct) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : distinct.typeParams) params.push_back(Term::binary(p));
    return checkedTuple({Term::atom("distinct"), Term::list(std::move(params)),
                         typeToTerm(distinct.backingType)},
                        schema::kDistinct, "distinct");
}

auto distinctFromSax(SaxReader& sax) -> kex::semantic::ImportedDistinctType {
    const auto arity = sax.beginTuple();
    if (arity != schema::kDistinct) throw EtfError("distinct: shape");
    if (sax.readAtom() != "distinct") throw EtfError("distinct: shape");
    kex::semantic::ImportedDistinctType distinct;
    const auto paramCount = sax.beginList();
    distinct.typeParams.reserve(paramCount);
    for (size_t i = 0; i < paramCount; ++i)
        distinct.typeParams.push_back(sax.readBinary());
    sax.endList();
    distinct.backingType = typeFromSax(sax);
    return distinct;
}

} // namespace

auto encodeSemanticType(const kex::semantic::TypePtr& type) -> TermPtr {
    return typeToTerm(type);
}
auto decodeSemanticType(SaxReader& sax) -> kex::semantic::TypePtr {
    return typeFromSax(sax);
}
auto encodeSignature(const kex::semantic::Signature& signature) -> TermPtr {
    return signatureToTerm(signature);
}
auto decodeSignature(SaxReader& sax) -> kex::semantic::Signature {
    return signatureFromSax(sax);
}

auto serializeImportedInterfaces(const kex::semantic::ImportedInterfaces& ifaces)
    -> std::vector<uint8_t> {
    using kex::semantic::ImportedModuleInterface;

    std::vector<TermPtr> modules;
    for (const auto& [name, module] : ifaces.modules) {
        std::vector<TermPtr> exports;
        for (const auto& [member, functions] : module.exports) {
            std::vector<TermPtr> fns;
            for (const auto& fn : functions) fns.push_back(functionToTerm(fn));
            exports.push_back(Term::tuple({Term::binary(member),
                                           Term::list(std::move(fns))}));
        }
        modules.push_back(checkedTuple(
            {Term::binary(name),
             checkedTuple({Term::binary(module.sourceModule),
                           Term::binary(module.backendModule),
                           Term::boolean(module.automaticImport),
                           Term::boolean(module.isCapability),
                           Term::boolean(module.compiledApart),
                           Term::list(std::move(exports))},
                          schema::kModuleBody, "module body")},
            schema::kModulePair, "module pair"));
    }

    std::vector<TermPtr> receivers;
    for (const auto& [name, functions] : ifaces.receiverFunctions) {
        std::vector<TermPtr> fns;
        for (const auto& fn : functions) fns.push_back(functionToTerm(fn));
        receivers.push_back(Term::tuple({Term::binary(name),
                                         Term::list(std::move(fns))}));
    }

    std::vector<TermPtr> conformances;
    for (const auto& c : ifaces.traitConformances)
        conformances.push_back(Term::tuple({Term::binary(c.typeName),
                                            Term::binary(c.traitName)}));

    std::vector<TermPtr> adts;
    for (const auto& adt : ifaces.adts) adts.push_back(adtToTerm(adt));

    std::vector<TermPtr> recordArities;
    for (const auto& [name, arity] : ifaces.recordArities)
        recordArities.push_back(Term::tuple({Term::binary(name),
                                             Term::integer(static_cast<int64_t>(arity))}));

    std::vector<TermPtr> recordFieldNames;
    for (const auto& [record, fields] : ifaces.recordFieldNames) {
        std::vector<TermPtr> names;
        for (const auto& f : fields) names.push_back(Term::binary(f));
        recordFieldNames.push_back(Term::tuple({Term::binary(record),
                                                Term::list(std::move(names))}));
    }

    std::vector<TermPtr> recordFields;
    for (const auto& [record, fields] : ifaces.recordFields) {
        std::vector<TermPtr> pairs;
        for (const auto& [field, type] : fields)
            pairs.push_back(Term::tuple({Term::binary(field), typeToTerm(type)}));
        recordFields.push_back(Term::tuple({Term::binary(record),
                                            Term::list(std::move(pairs))}));
    }

    std::vector<TermPtr> typeAliases;
    for (const auto& [name, type] : ifaces.typeAliases)
        typeAliases.push_back(Term::tuple({Term::binary(name), typeToTerm(type)}));

    std::vector<TermPtr> typeNames;
    for (const auto& name : ifaces.typeNames) typeNames.push_back(Term::binary(name));

    std::vector<TermPtr> distinctTypes;
    for (const auto& [name, distinct] : ifaces.distinctTypes)
        distinctTypes.push_back(Term::tuple({Term::binary(name),
                                             distinctToTerm(distinct)}));

    std::vector<TermPtr> traits;
    for (const auto& trait : ifaces.traits) traits.push_back(traitToTerm(trait));

    auto root = checkedTuple(
        {
            Term::atom("ifaces"),
            Term::integer(kMagic),
            Term::integer(kVersion),
            Term::list(std::move(modules)),
            Term::list(std::move(receivers)),
            Term::list(std::move(conformances)),
            Term::list(std::move(adts)),
            Term::list(std::move(recordArities)),
            Term::list(std::move(recordFieldNames)),
            Term::list(std::move(recordFields)),
            Term::list(std::move(typeAliases)),
            Term::list(std::move(typeNames)),
            Term::list(std::move(distinctTypes)),
            Term::list(std::move(traits)),
        },
        schema::kIfaces, "ifaces");
    return encodeEtf(root);
}

auto deserializeImportedInterfaces(const std::vector<uint8_t>& bytes,
                                   CacheMissReason* reason)
    -> std::optional<kex::semantic::ImportedInterfaces> {
    const bool timed = kex::timings::enabled();
    const auto note = [&](CacheMissReason r) {
        if (reason) *reason = r;
    };
    const auto t0 = std::chrono::steady_clock::now();
    try {
        SaxReader sax(bytes.data(), bytes.size());
        const auto arity = sax.beginTuple();
        if (arity != schema::kIfaces) {
            note(CacheMissReason::Corrupt);
            return std::nullopt;
        }
        if (sax.readAtom() != "ifaces") {
            note(CacheMissReason::Corrupt);
            return std::nullopt;
        }
        if (sax.readInt() != kMagic) {
            note(CacheMissReason::BadMagic);
            return std::nullopt;
        }
        if (sax.readInt() != kVersion) {
            note(CacheMissReason::BadVersion);
            return std::nullopt;
        }

        kex::semantic::ImportedInterfaces ifaces;

        const auto moduleCount = sax.beginList();
        ifaces.modules.reserve(moduleCount);
        for (size_t i = 0; i < moduleCount; ++i) {
            const auto pairArity = sax.beginTuple();
            if (pairArity != schema::kModulePair)
                throw EtfError("module pair: shape");
            auto name = sax.readBinary();
            const auto bodyArity = sax.beginTuple();
            if (bodyArity != schema::kModuleBody)
                throw EtfError("module body: shape");
            kex::semantic::ImportedModuleInterface module;
            module.sourceModule = sax.readBinary();
            module.backendModule = sax.readBinary();
            module.automaticImport = sax.readBool();
            module.isCapability = sax.readBool();
            module.compiledApart = sax.readBool();
            const auto exportCount = sax.beginList();
            module.exports.reserve(exportCount);
            for (size_t j = 0; j < exportCount; ++j) {
                sax.beginTuple();
                auto member = sax.readBinary();
                std::vector<kex::semantic::ImportedFunction> fns;
                const auto fnCount = sax.beginList();
                fns.reserve(fnCount);
                for (size_t k = 0; k < fnCount; ++k)
                    fns.push_back(functionFromSax(sax));
                sax.endList();
                module.exports[std::move(member)] = std::move(fns);
            }
            sax.endList();
            ifaces.modules[std::move(name)] = std::move(module);
        }
        sax.endList();

        const auto recvCount = sax.beginList();
        ifaces.receiverFunctions.reserve(recvCount);
        for (size_t i = 0; i < recvCount; ++i) {
            sax.beginTuple();
            auto name = sax.readBinary();
            std::vector<kex::semantic::ImportedFunction> fns;
            const auto fnCount = sax.beginList();
            fns.reserve(fnCount);
            for (size_t j = 0; j < fnCount; ++j)
                fns.push_back(functionFromSax(sax));
            sax.endList();
            ifaces.receiverFunctions[std::move(name)] = std::move(fns);
        }
        sax.endList();

        const auto conformanceCount = sax.beginList();
        ifaces.traitConformances.reserve(conformanceCount);
        for (size_t i = 0; i < conformanceCount; ++i) {
            sax.beginTuple();
            auto typeName = sax.readBinary();
            auto traitName = sax.readBinary();
            ifaces.traitConformances.push_back(
                {std::move(typeName), std::move(traitName)});
        }
        sax.endList();

        const auto adtCount = sax.beginList();
        ifaces.adts.reserve(adtCount);
        for (size_t i = 0; i < adtCount; ++i)
            ifaces.adts.push_back(adtFromSax(sax));
        sax.endList();

        const auto recordArityCount = sax.beginList();
        ifaces.recordArities.reserve(recordArityCount);
        for (size_t i = 0; i < recordArityCount; ++i) {
            sax.beginTuple();
            auto name = sax.readBinary();
            const auto value = static_cast<size_t>(sax.readInt());
            ifaces.recordArities[std::move(name)] = value;
        }
        sax.endList();

        const auto fieldNameCount = sax.beginList();
        ifaces.recordFieldNames.reserve(fieldNameCount);
        for (size_t i = 0; i < fieldNameCount; ++i) {
            sax.beginTuple();
            auto record = sax.readBinary();
            std::unordered_set<std::string> fields;
            const auto fieldCount = sax.beginList();
            fields.reserve(fieldCount);
            for (size_t j = 0; j < fieldCount; ++j)
                fields.insert(sax.readBinary());
            sax.endList();
            ifaces.recordFieldNames[std::move(record)] = std::move(fields);
        }
        sax.endList();

        const auto recordFieldCount = sax.beginList();
        ifaces.recordFields.reserve(recordFieldCount);
        for (size_t i = 0; i < recordFieldCount; ++i) {
            sax.beginTuple();
            auto record = sax.readBinary();
            std::unordered_map<std::string, TypePtr> fields;
            const auto fieldCount = sax.beginList();
            fields.reserve(fieldCount);
            for (size_t j = 0; j < fieldCount; ++j) {
                sax.beginTuple();
                auto field = sax.readBinary();
                fields[std::move(field)] = typeFromSax(sax);
            }
            sax.endList();
            ifaces.recordFields[std::move(record)] = std::move(fields);
        }
        sax.endList();

        const auto aliasCount = sax.beginList();
        ifaces.typeAliases.reserve(aliasCount);
        for (size_t i = 0; i < aliasCount; ++i) {
            sax.beginTuple();
            auto name = sax.readBinary();
            ifaces.typeAliases[std::move(name)] = typeFromSax(sax);
        }
        sax.endList();

        const auto typeNameCount = sax.beginList();
        ifaces.typeNames.reserve(typeNameCount);
        for (size_t i = 0; i < typeNameCount; ++i)
            ifaces.typeNames.insert(sax.readBinary());
        sax.endList();

        const auto distinctCount = sax.beginList();
        ifaces.distinctTypes.reserve(distinctCount);
        for (size_t i = 0; i < distinctCount; ++i) {
            sax.beginTuple();
            auto name = sax.readBinary();
            ifaces.distinctTypes[std::move(name)] = distinctFromSax(sax);
        }
        sax.endList();

        const auto traitCount = sax.beginList();
        ifaces.traits.reserve(traitCount);
        for (size_t i = 0; i < traitCount; ++i)
            ifaces.traits.push_back(traitFromSax(sax));
        sax.endList();

        if (timed) {
            const auto t1 = std::chrono::steady_clock::now();
            std::fprintf(stderr, "      [cache] sax %.1fms\n",
                         std::chrono::duration<double, std::milli>(t1 - t0)
                             .count());
        }
        return ifaces;
    } catch (const std::exception&) {
        note(CacheMissReason::Corrupt);
        return std::nullopt;
    }
}

auto cacheMissReasonName(CacheMissReason reason) -> const char* {
    switch (reason) {
    case CacheMissReason::BadMagic:   return "bad-magic";
    case CacheMissReason::BadVersion: return "bad-version";
    case CacheMissReason::Corrupt:    return "corrupt";
    }
    return "corrupt";
}

auto pruneInterfaceCacheDirectory(const std::filesystem::path& dir,
                                  const std::filesystem::path& keep,
                                  size_t keepNewest) -> void {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return;
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    std::vector<Entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return;
        if (!entry.is_regular_file(ec) || ec) continue;
        if (entry.path().extension() != ".bin") continue;
        if (entry.path().filename() == keep.filename()) continue;
        const auto mtime = std::filesystem::last_write_time(entry.path(), ec);
        if (ec) continue;
        entries.push_back({entry.path(), mtime});
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.mtime > b.mtime; });
    // `keep` holds one slot: the directory converges to `keepNewest` files.
    const size_t budget = keepNewest > 0 ? keepNewest - 1 : 0;
    for (size_t i = budget; i < entries.size(); i++)
        std::filesystem::remove(entries[i].path, ec);
}

} // namespace kex::beam

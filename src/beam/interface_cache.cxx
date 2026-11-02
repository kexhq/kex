#include "interface_cache.hxx"

#include "etf.hxx"

#include "../semantic/imported_interfaces.hxx"
#include "../semantic/traits.hxx"
#include "../semantic/types.hxx"

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

auto isTuple(const TermPtr& t) -> bool {
    return std::holds_alternative<Term::Tuple>(t->value);
}

constexpr int kMagic = 0x4b584946;      // "KXIF"
constexpr int kVersion = 1;

// ---- Type <-> Term ---------------------------------------------------------

auto typeToTerm(const TypePtr& t) -> TermPtr;

auto listOf(const std::vector<TypePtr>& types) -> TermPtr {
    std::vector<TermPtr> elems;
    elems.reserve(types.size());
    for (const auto& t : types) elems.push_back(typeToTerm(t));
    return Term::list(std::move(elems));
}

auto primitiveKindAtom(PrimitiveType::Kind k) -> const char* {
    switch (k) {
    case PrimitiveType::Integer: return "Integer";
    case PrimitiveType::Char:    return "Char";
    case PrimitiveType::String:  return "String";
    case PrimitiveType::Bool:    return "Bool";
    case PrimitiveType::Atom:    return "Atom";
    case PrimitiveType::Unit:    return "Unit";
    }
    return "Unit";
}

auto typeToTerm(const TypePtr& t) -> TermPtr {
    if (!t) return Term::atom("unknown");
    return std::visit([&](const auto& node) -> TermPtr {
        using K = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<K, PrimitiveType>) {
            return Term::tuple({Term::atom("primitive"),
                                Term::atom(primitiveKindAtom(node.kind)),
                                Term::binary(node.atomName)});
        } else if constexpr (std::is_same_v<K, SizedIntType>) {
            return Term::tuple({Term::atom("sized_int"),
                                Term::integer(node.bits),
                                Term::atom(node.isSigned ? "true" : "false")});
        } else if constexpr (std::is_same_v<K, SizedFloatType>) {
            return Term::tuple({Term::atom("sized_float"),
                                Term::integer(node.bits)});
        } else if constexpr (std::is_same_v<K, NamedType>) {
            return Term::tuple({Term::atom("named"), Term::binary(node.name),
                                listOf(node.typeArgs)});
        } else if constexpr (std::is_same_v<K, FuncType>) {
            return Term::tuple({Term::atom("func"), listOf(node.params),
                                typeToTerm(node.result),
                                Term::atom(node.block ? "true" : "false")});
        } else if constexpr (std::is_same_v<K, TupleType>) {
            return Term::tuple({Term::atom("tuple"), listOf(node.elements)});
        } else if constexpr (std::is_same_v<K, ListType>) {
            return Term::tuple({Term::atom("list"), typeToTerm(node.element)});
        } else if constexpr (std::is_same_v<K, MapType>) {
            return Term::tuple({Term::atom("map"), typeToTerm(node.key),
                                typeToTerm(node.value)});
        } else if constexpr (std::is_same_v<K, OptionalType>) {
            return Term::tuple({Term::atom("optional"),
                                typeToTerm(node.inner)});
        } else if constexpr (std::is_same_v<K, UnionType>) {
            return Term::tuple({Term::atom("union"), listOf(node.members)});
        } else if constexpr (std::is_same_v<K, IntersectionType>) {
            return Term::tuple({Term::atom("intersection"),
                                listOf(node.members)});
        } else if constexpr (std::is_same_v<K, RecordType>) {
            std::vector<TermPtr> fields;
            for (const auto& [name, fieldType] : node.fields)
                fields.push_back(Term::tuple({Term::binary(name),
                                              typeToTerm(fieldType)}));
            return Term::tuple({Term::atom("record"),
                                Term::list(std::move(fields))});
        } else if constexpr (std::is_same_v<K, TypeVar>) {
            return Term::tuple({Term::atom("typevar"), Term::integer(node.id)});
        } else if constexpr (std::is_same_v<K, UnknownType>) {
            return Term::atom("unknown");
        } else if constexpr (std::is_same_v<K, VoidType>) {
            return Term::tuple({Term::atom("void"), Term::binary(node.reason)});
        } else if constexpr (std::is_same_v<K, ConstrainedType>) {
            return Term::tuple({Term::atom("constrained"),
                                Term::binary(node.varName),
                                Term::binary(node.traitName),
                                Term::integer(node.genericId)});
        }
        return Term::atom("unknown");
    }, t->kind);
}

// Throws EtfError on any malformed term.
auto typeFromTerm(const TermPtr& term) -> TypePtr {
    if (term->isAtom("unknown")) return std::make_shared<Type>(Type{UnknownType{}});
    if (!isTuple(term)) throw EtfError("type: expected tuple");
    const auto& elems = term->asTuple();
    if (elems.empty()) throw EtfError("type: empty tuple");
    const auto tag = elems[0]->asAtom();
    if (tag == "primitive") {
        if (elems.size() != 3) throw EtfError("primitive: arity");
        PrimitiveType::Kind kind;
        const auto name = elems[1]->asAtom();
        if (name == "Integer") kind = PrimitiveType::Integer;
        else if (name == "Char") kind = PrimitiveType::Char;
        else if (name == "String") kind = PrimitiveType::String;
        else if (name == "Bool") kind = PrimitiveType::Bool;
        else if (name == "Atom") kind = PrimitiveType::Atom;
        else if (name == "Unit") kind = PrimitiveType::Unit;
        else throw EtfError("primitive: bad kind");
        return std::make_shared<Type>(Type{PrimitiveType{
            kind, elems[2]->asBinaryStr()}});
    }
    if (tag == "sized_int") {
        if (elems.size() != 3) throw EtfError("sized_int: arity");
        return std::make_shared<Type>(Type{SizedIntType{
            static_cast<int>(elems[1]->asInt()),
            elems[2]->isAtom("true")}});
    }
    if (tag == "sized_float") {
        if (elems.size() != 2) throw EtfError("sized_float: arity");
        return std::make_shared<Type>(Type{SizedFloatType{
            static_cast<int>(elems[1]->asInt())}});
    }
    if (tag == "named") {
        if (elems.size() != 3) throw EtfError("named: arity");
        NamedType named{elems[1]->asBinaryStr(), {}};
        for (const auto& arg : elems[2]->asList())
            named.typeArgs.push_back(typeFromTerm(arg));
        return std::make_shared<Type>(Type{std::move(named)});
    }
    if (tag == "func") {
        if (elems.size() != 4) throw EtfError("func: arity");
        FuncType fn;
        for (const auto& param : elems[1]->asList())
            fn.params.push_back(typeFromTerm(param));
        fn.result = typeFromTerm(elems[2]);
        fn.block = elems[3]->isAtom("true");
        return std::make_shared<Type>(Type{std::move(fn)});
    }
    if (tag == "tuple") {
        if (elems.size() != 2) throw EtfError("tuple: arity");
        TupleType tuple;
        for (const auto& element : elems[1]->asList())
            tuple.elements.push_back(typeFromTerm(element));
        return std::make_shared<Type>(Type{std::move(tuple)});
    }
    if (tag == "list") {
        if (elems.size() != 2) throw EtfError("list: arity");
        return std::make_shared<Type>(Type{ListType{typeFromTerm(elems[1])}});
    }
    if (tag == "map") {
        if (elems.size() != 3) throw EtfError("map: arity");
        return std::make_shared<Type>(Type{MapType{typeFromTerm(elems[1]),
                                                   typeFromTerm(elems[2])}});
    }
    if (tag == "optional") {
        if (elems.size() != 2) throw EtfError("optional: arity");
        return std::make_shared<Type>(Type{OptionalType{typeFromTerm(elems[1])}});
    }
    if (tag == "union") {
        if (elems.size() != 2) throw EtfError("union: arity");
        UnionType unionType;
        for (const auto& member : elems[1]->asList())
            unionType.members.push_back(typeFromTerm(member));
        return std::make_shared<Type>(Type{std::move(unionType)});
    }
    if (tag == "intersection") {
        if (elems.size() != 2) throw EtfError("intersection: arity");
        IntersectionType intersection;
        for (const auto& member : elems[1]->asList())
            intersection.members.push_back(typeFromTerm(member));
        return std::make_shared<Type>(Type{std::move(intersection)});
    }
    if (tag == "record") {
        if (elems.size() != 2) throw EtfError("record: arity");
        RecordType record;
        for (const auto& field : elems[1]->asList()) {
            const auto& pair = field->asTuple();
            record.fields.emplace_back(pair[0]->asBinaryStr(),
                                       typeFromTerm(pair[1]));
        }
        return std::make_shared<Type>(Type{std::move(record)});
    }
    if (tag == "typevar") {
        if (elems.size() != 2) throw EtfError("typevar: arity");
        return std::make_shared<Type>(Type{TypeVar{
            static_cast<int>(elems[1]->asInt())}});
    }
    if (tag == "void") {
        if (elems.size() != 2) throw EtfError("void: arity");
        return std::make_shared<Type>(Type{VoidType{elems[1]->asBinaryStr()}});
    }
    if (tag == "constrained") {
        if (elems.size() != 4) throw EtfError("constrained: arity");
        return std::make_shared<Type>(Type{ConstrainedType{
            elems[1]->asBinaryStr(), elems[2]->asBinaryStr(),
            static_cast<int>(elems[3]->asInt())}});
    }
    throw EtfError("type: unknown tag " + tag);
}

// ---- Signature <-> Term ----------------------------------------------------

auto signatureToTerm(const kex::semantic::Signature& sig) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : sig.params) params.push_back(typeToTerm(p));
    TermPtr required = sig.requiredParams
        ? Term::tuple({Term::atom("some"),
                       Term::integer(static_cast<int64_t>(*sig.requiredParams))})
        : Term::atom("none");
    std::vector<TermPtr> names;
    for (const auto& n : sig.paramNames) names.push_back(Term::binary(n));
    return Term::tuple({Term::atom("signature"), Term::binary(sig.name),
                        Term::list(std::move(params)), typeToTerm(sig.result),
                        Term::atom(sig.isFoul ? "true" : "false"), required,
                        Term::list(std::move(names)), Term::binary(sig.makeModule),
                        Term::binary(sig.declaringModule)});
}

auto signatureFromTerm(const TermPtr& term) -> kex::semantic::Signature {
    const auto& elems = term->asTuple();
    if (elems.size() != 9 || !elems[0]->isAtom("signature"))
        throw EtfError("signature: shape");
    kex::semantic::Signature sig;
    sig.name = elems[1]->asBinaryStr();
    for (const auto& p : elems[2]->asList()) sig.params.push_back(typeFromTerm(p));
    sig.result = typeFromTerm(elems[3]);
    sig.isFoul = elems[4]->isAtom("true");
    if (isTuple(elems[5])) {
        const auto& req = elems[5]->asTuple();
        sig.requiredParams = static_cast<size_t>(req[1]->asInt());
    }
    for (const auto& n : elems[6]->asList())
        sig.paramNames.push_back(n->asBinaryStr());
    sig.makeModule = elems[7]->asBinaryStr();
    sig.declaringModule = elems[8]->asBinaryStr();
    return sig;
}

// ---- ImportedFunction <-> Term ---------------------------------------------

auto functionToTerm(const kex::semantic::ImportedFunction& fn) -> TermPtr {
    std::vector<TermPtr> names;
    for (const auto& n : fn.paramNames) names.push_back(Term::binary(n));
    std::vector<TermPtr> defaults;
    for (const auto& d : fn.defaultSources) defaults.push_back(Term::binary(d));
    return Term::tuple({Term::atom("fn"), Term::binary(fn.sourceName),
                        Term::binary(fn.backendFunction),
                        Term::binary(fn.backendModule),
                        Term::integer(fn.backendArity),
                        Term::list(std::move(names)),
                        Term::list(std::move(defaults)),
                        signatureToTerm(fn.signature),
                        Term::binary(fn.sourceModule),
                        Term::atom(fn.isReceiverMethod ? "true" : "false"),
                        Term::atom(fn.visibleByReceiver ? "true" : "false"),
                        Term::atom(fn.isConstructor ? "true" : "false"),
                        Term::atom(fn.remoteSlot ? "true" : "false"),
                        Term::binary(fn.constructorOwner)});
}

auto functionFromTerm(const TermPtr& term) -> kex::semantic::ImportedFunction {
    const auto& elems = term->asTuple();
    if (elems.size() != 14 || !elems[0]->isAtom("fn"))
        throw EtfError("fn: shape");
    kex::semantic::ImportedFunction fn;
    fn.sourceName = elems[1]->asBinaryStr();
    fn.backendFunction = elems[2]->asBinaryStr();
    fn.backendModule = elems[3]->asBinaryStr();
    fn.backendArity = static_cast<int>(elems[4]->asInt());
    for (const auto& n : elems[5]->asList()) fn.paramNames.push_back(n->asBinaryStr());
    for (const auto& d : elems[6]->asList()) fn.defaultSources.push_back(d->asBinaryStr());
    fn.signature = signatureFromTerm(elems[7]);
    fn.sourceModule = elems[8]->asBinaryStr();
    fn.isReceiverMethod = elems[9]->isAtom("true");
    fn.visibleByReceiver = elems[10]->isAtom("true");
    fn.isConstructor = elems[11]->isAtom("true");
    fn.remoteSlot = elems[12]->isAtom("true");
    fn.constructorOwner = elems[13]->asBinaryStr();
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
    return Term::tuple({Term::atom("trait"), Term::binary(trait.name),
                        Term::list(std::move(required)),
                        Term::list(std::move(defaults))});
}

auto traitFromTerm(const TermPtr& term) -> kex::semantic::TraitDef {
    const auto& elems = term->asTuple();
    if (elems.size() != 4 || !elems[0]->isAtom("trait"))
        throw EtfError("trait: shape");
    kex::semantic::TraitDef trait;
    trait.name = elems[1]->asBinaryStr();
    for (const auto& sig : elems[2]->asList())
        trait.requiredMethods.push_back(signatureFromTerm(sig));
    for (const auto& name : elems[3]->asList())
        trait.defaultMethods.push_back(name->asBinaryStr());
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
    return Term::tuple({Term::atom("adt"), Term::binary(adt.name),
                        Term::list(std::move(ctors)), Term::list(std::move(arities)),
                        Term::integer(static_cast<int64_t>(adt.typeParamCount)),
                        Term::list(std::move(slots)), Term::list(std::move(paramNames)),
                        Term::list(std::move(paramTypes))});
}

auto adtFromTerm(const TermPtr& term) -> kex::semantic::ImportedADT {
    const auto& elems = term->asTuple();
    if (elems.size() != 8 || !elems[0]->isAtom("adt"))
        throw EtfError("adt: shape");
    kex::semantic::ImportedADT adt;
    adt.name = elems[1]->asBinaryStr();
    for (const auto& c : elems[2]->asList()) adt.constructors.push_back(c->asBinaryStr());
    for (const auto& pair : elems[3]->asList()) {
        const auto& p = pair->asTuple();
        adt.constructorArities[p[0]->asBinaryStr()] = static_cast<int>(p[1]->asInt());
    }
    adt.typeParamCount = static_cast<size_t>(elems[4]->asInt());
    for (const auto& pair : elems[5]->asList()) {
        const auto& p = pair->asTuple();
        std::vector<int> idx;
        for (const auto& i : p[1]->asList()) idx.push_back(static_cast<int>(i->asInt()));
        adt.constructorTypeParamSlots[p[0]->asBinaryStr()] = std::move(idx);
    }
    for (const auto& n : elems[6]->asList()) adt.typeParamNames.push_back(n->asBinaryStr());
    for (const auto& pair : elems[7]->asList()) {
        const auto& p = pair->asTuple();
        std::vector<TypePtr> types;
        for (const auto& t : p[1]->asList()) types.push_back(typeFromTerm(t));
        adt.constructorParamTypes[p[0]->asBinaryStr()] = std::move(types);
    }
    return adt;
}

auto distinctToTerm(const kex::semantic::ImportedDistinctType& distinct) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : distinct.typeParams) params.push_back(Term::binary(p));
    return Term::tuple({Term::atom("distinct"), Term::list(std::move(params)),
                        typeToTerm(distinct.backingType)});
}

auto distinctFromTerm(const TermPtr& term) -> kex::semantic::ImportedDistinctType {
    const auto& elems = term->asTuple();
    if (elems.size() != 3 || !elems[0]->isAtom("distinct"))
        throw EtfError("distinct: shape");
    kex::semantic::ImportedDistinctType distinct;
    for (const auto& p : elems[1]->asList())
        distinct.typeParams.push_back(p->asBinaryStr());
    distinct.backingType = typeFromTerm(elems[2]);
    return distinct;
}

} // namespace

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
        modules.push_back(Term::tuple({Term::binary(name),
            Term::tuple({Term::binary(module.sourceModule),
                         Term::binary(module.backendModule),
                         Term::atom(module.automaticImport ? "true" : "false"),
                         Term::atom(module.isCapability ? "true" : "false"),
                         Term::atom(module.compiledApart ? "true" : "false"),
                         Term::list(std::move(exports))})}));
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

    auto root = Term::tuple({
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
    });
    return encodeEtf(root);
}

auto deserializeImportedInterfaces(const std::vector<uint8_t>& bytes)
    -> std::optional<kex::semantic::ImportedInterfaces> {
    try {
        auto root = decodeEtf(bytes);
        const auto& elems = root->asTuple();
        if (elems.size() != 14 || !elems[0]->isAtom("ifaces")) return std::nullopt;
        if (elems[1]->asInt() != kMagic) return std::nullopt;
        if (elems[2]->asInt() != kVersion) return std::nullopt;

        kex::semantic::ImportedInterfaces ifaces;

        for (const auto& mod : elems[3]->asList()) {
            const auto& pair = mod->asTuple();
            const auto& body = pair[1]->asTuple();
            kex::semantic::ImportedModuleInterface module;
            module.sourceModule = body[0]->asBinaryStr();
            module.backendModule = body[1]->asBinaryStr();
            module.automaticImport = body[2]->isAtom("true");
            module.isCapability = body[3]->isAtom("true");
            module.compiledApart = body[4]->isAtom("true");
            for (const auto& entry : body[5]->asList()) {
                const auto& e = entry->asTuple();
                std::vector<kex::semantic::ImportedFunction> fns;
                for (const auto& fn : e[1]->asList())
                    fns.push_back(functionFromTerm(fn));
                module.exports[e[0]->asBinaryStr()] = std::move(fns);
            }
            ifaces.modules[pair[0]->asBinaryStr()] = std::move(module);
        }

        for (const auto& recv : elems[4]->asList()) {
            const auto& pair = recv->asTuple();
            std::vector<kex::semantic::ImportedFunction> fns;
            for (const auto& fn : pair[1]->asList())
                fns.push_back(functionFromTerm(fn));
            ifaces.receiverFunctions[pair[0]->asBinaryStr()] = std::move(fns);
        }

        for (const auto& c : elems[5]->asList()) {
            const auto& p = c->asTuple();
            ifaces.traitConformances.push_back({p[0]->asBinaryStr(),
                                                p[1]->asBinaryStr()});
        }

        for (const auto& adt : elems[6]->asList())
            ifaces.adts.push_back(adtFromTerm(adt));

        for (const auto& entry : elems[7]->asList()) {
            const auto& p = entry->asTuple();
            ifaces.recordArities[p[0]->asBinaryStr()] =
                static_cast<size_t>(p[1]->asInt());
        }

        for (const auto& entry : elems[8]->asList()) {
            const auto& p = entry->asTuple();
            std::unordered_set<std::string> fields;
            for (const auto& f : p[1]->asList()) fields.insert(f->asBinaryStr());
            ifaces.recordFieldNames[p[0]->asBinaryStr()] = std::move(fields);
        }

        for (const auto& entry : elems[9]->asList()) {
            const auto& p = entry->asTuple();
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& pair : p[1]->asList()) {
                const auto& f = pair->asTuple();
                fields[f[0]->asBinaryStr()] = typeFromTerm(f[1]);
            }
            ifaces.recordFields[p[0]->asBinaryStr()] = std::move(fields);
        }

        for (const auto& entry : elems[10]->asList()) {
            const auto& p = entry->asTuple();
            ifaces.typeAliases[p[0]->asBinaryStr()] = typeFromTerm(p[1]);
        }

        for (const auto& name : elems[11]->asList())
            ifaces.typeNames.insert(name->asBinaryStr());

        for (const auto& entry : elems[12]->asList()) {
            const auto& p = entry->asTuple();
            ifaces.distinctTypes[p[0]->asBinaryStr()] = distinctFromTerm(p[1]);
        }

        for (const auto& trait : elems[13]->asList())
            ifaces.traits.push_back(traitFromTerm(trait));

        return ifaces;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace kex::beam

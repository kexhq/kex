#include "kexi.hxx"
#include "beam_file.hxx"
#include <cstring>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#elif !defined(__EMSCRIPTEN__)
#include <openssl/sha.h>
#endif

namespace kex::beam {

// ── KexiType constructors ────────────────────────────────────────────

auto kexiPrimitive(const std::string& name) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Primitive;
    t->name = name;
    return t;
}
auto kexiNamed(const std::string& name, std::vector<KexiTypePtr> args) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Named;
    t->name = name;
    t->typeArgs = std::move(args);
    return t;
}
auto kexiFunc(std::vector<KexiTypePtr> params, KexiTypePtr result) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Func;
    t->typeArgs = std::move(params);
    t->result = std::move(result);
    return t;
}
auto kexiTuple(std::vector<KexiTypePtr> elems) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Tuple;
    t->typeArgs = std::move(elems);
    return t;
}
auto kexiList(KexiTypePtr element) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::List;
    t->typeArgs = {std::move(element)};
    return t;
}
auto kexiMap(KexiTypePtr key, KexiTypePtr value) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Map;
    t->typeArgs = {std::move(key), std::move(value)};
    return t;
}
auto kexiOptional(KexiTypePtr inner) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Optional;
    t->typeArgs = {std::move(inner)};
    return t;
}
auto kexiUnion(std::vector<KexiTypePtr> members) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Union;
    t->typeArgs = std::move(members);
    return t;
}
auto kexiConstrained(const std::string& var, const std::string& trait) -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Constrained;
    t->name = var;
    t->traitName = trait;
    return t;
}
auto kexiNever() -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Never;
    return t;
}
auto kexiUnknown() -> KexiTypePtr {
    auto t = std::make_shared<KexiType>();
    t->kind = KexiType::Unknown;
    return t;
}

// ── ETF encoding helpers ─────────────────────────────────────────────

namespace {

auto typeToTerm(const KexiTypePtr& type) -> TermPtr {
    if (!type)
        return Term::tuple({Term::atom("unknown")});
    switch (type->kind) {
    case KexiType::Primitive:
        return Term::tuple({Term::atom("prim"), Term::atom(type->name)});
    case KexiType::Named: {
        std::vector<TermPtr> args;
        for (const auto& a : type->typeArgs) args.push_back(typeToTerm(a));
        return Term::tuple({Term::atom("named"), Term::binary(type->name),
                            Term::list(std::move(args))});
    }
    case KexiType::Func: {
        std::vector<TermPtr> params;
        for (const auto& p : type->typeArgs) params.push_back(typeToTerm(p));
        return Term::tuple({Term::atom("func"), Term::list(std::move(params)),
                            typeToTerm(type->result)});
    }
    case KexiType::Tuple: {
        std::vector<TermPtr> elems;
        for (const auto& e : type->typeArgs) elems.push_back(typeToTerm(e));
        return Term::tuple({Term::atom("tuple"), Term::list(std::move(elems))});
    }
    case KexiType::List:
        return Term::tuple({Term::atom("list"),
                            typeToTerm(type->typeArgs.empty() ? nullptr : type->typeArgs[0])});
    case KexiType::Map:
        return Term::tuple({Term::atom("map"),
                            typeToTerm(type->typeArgs.size() > 0 ? type->typeArgs[0] : nullptr),
                            typeToTerm(type->typeArgs.size() > 1 ? type->typeArgs[1] : nullptr)});
    case KexiType::Optional:
        return Term::tuple({Term::atom("optional"),
                            typeToTerm(type->typeArgs.empty() ? nullptr : type->typeArgs[0])});
    case KexiType::Union: {
        std::vector<TermPtr> members;
        for (const auto& m : type->typeArgs) members.push_back(typeToTerm(m));
        return Term::tuple({Term::atom("union"), Term::list(std::move(members))});
    }
    case KexiType::Constrained:
        return Term::tuple({Term::atom("constrained"),
                            Term::binary(type->name), Term::binary(type->traitName)});
    case KexiType::Never:
        return Term::tuple({Term::atom("never")});
    case KexiType::Unknown:
        return Term::tuple({Term::atom("unknown")});
    }
    return Term::tuple({Term::atom("unknown")});
}

auto termToType(const TermPtr& term) -> KexiTypePtr {
    if (!term) return kexiUnknown();
    auto& elems = term->asTuple();
    if (elems.empty()) return kexiUnknown();
    auto& tag = elems[0]->asAtom();
    if (tag == "prim" && elems.size() >= 2)
        return kexiPrimitive(elems[1]->asAtom());
    if (tag == "named" && elems.size() >= 3) {
        std::vector<KexiTypePtr> args;
        for (const auto& a : elems[2]->asList()) args.push_back(termToType(a));
        return kexiNamed(elems[1]->asBinaryStr(), std::move(args));
    }
    if (tag == "func" && elems.size() >= 3) {
        std::vector<KexiTypePtr> params;
        for (const auto& p : elems[1]->asList()) params.push_back(termToType(p));
        return kexiFunc(std::move(params), termToType(elems[2]));
    }
    if (tag == "tuple" && elems.size() >= 2) {
        std::vector<KexiTypePtr> es;
        for (const auto& e : elems[1]->asList()) es.push_back(termToType(e));
        return kexiTuple(std::move(es));
    }
    if (tag == "list" && elems.size() >= 2)
        return kexiList(termToType(elems[1]));
    if (tag == "map" && elems.size() >= 3)
        return kexiMap(termToType(elems[1]), termToType(elems[2]));
    if (tag == "optional" && elems.size() >= 2)
        return kexiOptional(termToType(elems[1]));
    if (tag == "union" && elems.size() >= 2) {
        std::vector<KexiTypePtr> ms;
        for (const auto& m : elems[1]->asList()) ms.push_back(termToType(m));
        return kexiUnion(std::move(ms));
    }
    if (tag == "constrained" && elems.size() >= 3)
        return kexiConstrained(elems[1]->asBinaryStr(), elems[2]->asBinaryStr());
    if (tag == "never") return kexiNever();
    return kexiUnknown();
}

auto hashToTerm(const Hash128& h) -> TermPtr {
    return Term::binary(std::vector<uint8_t>(h.begin(), h.end()));
}

auto termToHash(const TermPtr& term) -> Hash128 {
    Hash128 h{};
    auto& data = term->asBinary();
    size_t n = std::min(data.size(), size_t(16));
    for (size_t i = 0; i < n; i++) h[i] = data[i];
    return h;
}

auto exportToTerm(const KexiExport& exp, int version) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : exp.paramTypes) params.push_back(typeToTerm(p));
    std::vector<TermPtr> fields = {
        Term::binary(exp.name),
        Term::integer(exp.beamArity),
        exp.isFoul ? Term::atom("true") : Term::atom("false"),
        Term::list(std::move(params)),
        typeToTerm(exp.returnType),
    };
    if (version >= 2)
        fields.push_back(Term::binary(
            exp.beamFunction.empty() ? exp.name : exp.beamFunction));
    if (version >= 3) {
        std::vector<TermPtr> names;
        for (const auto& n : exp.paramNames) names.push_back(Term::binary(n));
        fields.push_back(Term::list(std::move(names)));
    }
    return Term::tuple(std::move(fields));
}

auto termToExport(const TermPtr& term) -> KexiExport {
    auto& t = term->asTuple();
    KexiExport e;
    e.name = t[0]->asBinaryStr();
    e.beamArity = static_cast<int>(t[1]->asInt());
    e.isFoul = t[2]->isAtom("true");
    for (const auto& p : t[3]->asList())
        e.paramTypes.push_back(termToType(p));
    e.returnType = termToType(t[4]);
    e.beamFunction = t.size() >= 6 ? t[5]->asBinaryStr() : e.name;
    if (t.size() >= 7)
        for (const auto& n : t[6]->asList())
            e.paramNames.push_back(n->asBinaryStr());
    return e;
}

auto constantToTerm(const KexiConstant& c) -> TermPtr {
    return Term::tuple({Term::binary(c.name), typeToTerm(c.type)});
}

auto termToConstant(const TermPtr& term) -> KexiConstant {
    auto& t = term->asTuple();
    return {t[0]->asBinaryStr(), termToType(t[1])};
}

auto typeExportToTerm(const KexiTypeExport& te) -> TermPtr {
    std::vector<TermPtr> gp, cs;
    for (const auto& g : te.genericParams) gp.push_back(Term::binary(g));
    for (const auto& c : te.constructors) cs.push_back(Term::binary(c));
    return Term::tuple({Term::binary(te.name),
                        Term::list(std::move(gp)),
                        Term::list(std::move(cs))});
}

auto termToTypeExport(const TermPtr& term) -> KexiTypeExport {
    auto& t = term->asTuple();
    KexiTypeExport te;
    te.name = t[0]->asBinaryStr();
    for (const auto& g : t[1]->asList()) te.genericParams.push_back(g->asBinaryStr());
    for (const auto& c : t[2]->asList()) te.constructors.push_back(c->asBinaryStr());
    return te;
}

auto methodToTerm(const KexiMethod& m, int version) -> TermPtr {
    std::vector<TermPtr> params;
    for (const auto& p : m.paramTypes) params.push_back(typeToTerm(p));
    std::vector<TermPtr> fields = {
        Term::binary(m.name),
        typeToTerm(m.receiverType),
        Term::integer(m.beamArity),
        m.isFoul ? Term::atom("true") : Term::atom("false"),
        Term::list(std::move(params)),
        typeToTerm(m.returnType),
        Term::binary(m.beamFunction),
        m.typeOnly ? Term::atom("true") : Term::atom("false"),
    };
    if (version >= 5) {
        std::vector<TermPtr> names;
        for (const auto& n : m.paramNames) names.push_back(Term::binary(n));
        fields.push_back(Term::list(std::move(names)));
    }
    return Term::tuple(std::move(fields));
}

auto termToMethod(const TermPtr& term) -> KexiMethod {
    auto& t = term->asTuple();
    KexiMethod m;
    m.name = t[0]->asBinaryStr();
    m.receiverType = termToType(t[1]);
    m.beamArity = static_cast<int>(t[2]->asInt());
    m.isFoul = t[3]->isAtom("true");
    for (const auto& p : t[4]->asList())
        m.paramTypes.push_back(termToType(p));
    m.returnType = termToType(t[5]);
    m.beamFunction = t[6]->asBinaryStr();
    m.typeOnly = t.size() >= 8 && t[7]->isAtom("true");
    if (t.size() >= 9)
        for (const auto& n : t[8]->asList())
            m.paramNames.push_back(n->asBinaryStr());
    return m;
}

auto companionToTerm(const KexiCompanion& c) -> TermPtr {
    return Term::tuple({Term::binary(c.beamAtom),
                        Term::binary(c.relativePath),
                        hashToTerm(c.expectedHash)});
}

auto termToCompanion(const TermPtr& term) -> KexiCompanion {
    auto& t = term->asTuple();
    return {t[0]->asBinaryStr(), t[1]->asBinaryStr(), termToHash(t[2])};
}

auto recordToTerm(const KexiRecord& r) -> TermPtr {
    std::vector<TermPtr> fields;
    for (const auto& f : r.fields)
        fields.push_back(Term::tuple({Term::binary(f.name), typeToTerm(f.type)}));
    return Term::tuple({Term::binary(r.name), Term::list(std::move(fields))});
}

auto termToRecord(const TermPtr& term) -> KexiRecord {
    auto& t = term->asTuple();
    KexiRecord r;
    r.name = t[0]->asBinaryStr();
    for (const auto& f : t[1]->asList()) {
        auto& fe = f->asTuple();
        r.fields.push_back({fe[0]->asBinaryStr(), termToType(fe[1])});
    }
    return r;
}

auto constructorToTerm(const KexiConstructor& c) -> TermPtr {
    std::vector<TermPtr> fieldNames;
    for (const auto& n : c.fieldNames) fieldNames.push_back(Term::binary(n));
    return Term::tuple({Term::binary(c.name),
                        Term::binary(c.tagAtom),
                        Term::list(std::move(fieldNames)),
                        Term::integer(c.arity)});
}

auto termToConstructor(const TermPtr& term) -> KexiConstructor {
    auto& t = term->asTuple();
    KexiConstructor c;
    c.name = t[0]->asBinaryStr();
    c.tagAtom = t[1]->asBinaryStr();
    for (const auto& n : t[2]->asList()) c.fieldNames.push_back(n->asBinaryStr());
    c.arity = static_cast<int>(t[3]->asInt());
    return c;
}

auto adtToTerm(const KexiADT& adt) -> TermPtr {
    std::vector<TermPtr> tps, cs;
    for (const auto& tp : adt.typeParams) tps.push_back(Term::binary(tp));
    for (const auto& c : adt.constructors) cs.push_back(constructorToTerm(c));
    return Term::tuple({Term::binary(adt.name),
                        Term::list(std::move(tps)),
                        Term::list(std::move(cs))});
}

auto termToADT(const TermPtr& term) -> KexiADT {
    auto& t = term->asTuple();
    KexiADT adt;
    adt.name = t[0]->asBinaryStr();
    for (const auto& tp : t[1]->asList()) adt.typeParams.push_back(tp->asBinaryStr());
    for (const auto& c : t[2]->asList()) adt.constructors.push_back(termToConstructor(c));
    return adt;
}

auto methodOwnershipToTerm(const KexiMethodOwnership& mo) -> TermPtr {
    return Term::tuple({Term::binary(mo.methodName), Term::binary(mo.beamFunction)});
}

auto termToMethodOwnership(const TermPtr& term) -> KexiMethodOwnership {
    auto& t = term->asTuple();
    return {t[0]->asBinaryStr(), t[1]->asBinaryStr()};
}

auto conformanceToTerm(const KexiTraitConformance& c) -> TermPtr {
    return Term::tuple({Term::binary(c.typeName), Term::binary(c.traitName)});
}

auto termToConformance(const TermPtr& term) -> KexiTraitConformance {
    auto& t = term->asTuple();
    return {t[0]->asBinaryStr(), t[1]->asBinaryStr()};
}

auto traitDefToTerm(const KexiTraitDef& td) -> TermPtr {
    std::vector<TermPtr> methods;
    for (const auto& m : td.requiredMethods) {
        if (m.isFoul)
            methods.push_back(Term::tuple({Term::binary(m.name), Term::atom("foul")}));
        else
            methods.push_back(Term::binary(m.name));
    }
    return Term::tuple({Term::binary(td.name), Term::list(std::move(methods))});
}

auto termToTraitDef(const TermPtr& term) -> KexiTraitDef {
    auto& t = term->asTuple();
    KexiTraitDef td;
    td.name = t[0]->asBinaryStr();
    for (const auto& m : t[1]->asList()) {
        if (auto* tup = std::get_if<Term::Tuple>(&m->value); tup && tup->elements.size() == 2) {
            td.requiredMethods.push_back({tup->elements[0]->asBinaryStr(), true});
        } else {
            td.requiredMethods.push_back({m->asBinaryStr(), false});
        }
    }
    return td;
}

auto stringsToTerm(const std::vector<std::string>& values) -> TermPtr {
    std::vector<TermPtr> terms;
    terms.reserve(values.size());
    for (const auto& value : values) terms.push_back(Term::binary(value));
    return Term::list(std::move(terms));
}

auto termToStrings(const TermPtr& term) -> std::vector<std::string> {
    std::vector<std::string> values;
    for (const auto& value : term->asList())
        values.push_back(value->asBinaryStr());
    return values;
}

auto packageToTerm(const kex::module::PackageMetadata& package) -> TermPtr {
    return Term::map({
        {Term::atom("id"), Term::binary(package.id)},
        {Term::atom("unit_ids"), stringsToTerm(package.unitIds)},
        {Term::atom("automatic_imports"), stringsToTerm(package.automaticImports)},
        {Term::atom("receiver_providers"), stringsToTerm(package.receiverProviders)},
    });
}

auto termToPackage(const TermPtr& term) -> kex::module::PackageMetadata {
    kex::module::PackageMetadata package;
    if (auto id = term->mapGet("id")) package.id = id->asBinaryStr();
    if (auto units = term->mapGet("unit_ids"))
        package.unitIds = termToStrings(units);
    if (auto imports = term->mapGet("automatic_imports"))
        package.automaticImports = termToStrings(imports);
    if (auto providers = term->mapGet("receiver_providers"))
        package.receiverProviders = termToStrings(providers);
    return package;
}

// Build the full KexI ETF term WITHOUT the hash field, for hashing.
auto chunkToTermWithoutHash(const KexiChunk& chunk) -> TermPtr {
    // Type interface
    std::vector<TermPtr> exports, constants, types, methods;
    for (const auto& e : chunk.typeInterface.exports)
        exports.push_back(exportToTerm(e, chunk.version));
    for (const auto& c : chunk.typeInterface.constants) constants.push_back(constantToTerm(c));
    for (const auto& t : chunk.typeInterface.types) types.push_back(typeExportToTerm(t));
    for (const auto& m : chunk.typeInterface.methods)
        methods.push_back(methodToTerm(m, chunk.version));

    auto typeIface = Term::map({
        {Term::atom("exports"), Term::list(std::move(exports))},
        {Term::atom("constants"), Term::list(std::move(constants))},
        {Term::atom("types"), Term::list(std::move(types))},
        {Term::atom("methods"), Term::list(std::move(methods))},
    });

    // Structural metadata
    std::vector<TermPtr> companions, records, adts, ownership, pubExports;
    for (const auto& c : chunk.metadata.companions) companions.push_back(companionToTerm(c));
    for (const auto& r : chunk.metadata.records) records.push_back(recordToTerm(r));
    for (const auto& a : chunk.metadata.adts) adts.push_back(adtToTerm(a));
    for (const auto& mo : chunk.metadata.methodOwnership) ownership.push_back(methodOwnershipToTerm(mo));
    for (const auto& e : chunk.metadata.publicExports) pubExports.push_back(Term::binary(e));

    auto structural = Term::map({
        {Term::atom("module"), Term::binary(chunk.metadata.moduleAtom)},
        {Term::atom("foul"), chunk.metadata.isFoul ? Term::atom("true") : Term::atom("false")},
        {Term::atom("role"), chunk.metadata.role == KexiModuleRole::Entry
                                 ? Term::atom("entry") : Term::atom("companion")},
        {Term::atom("entry_back_pointer"), Term::binary(chunk.metadata.entryBackPointer)},
        {Term::atom("companions"), Term::list(std::move(companions))},
        {Term::atom("records"), Term::list(std::move(records))},
        {Term::atom("adts"), Term::list(std::move(adts))},
        {Term::atom("method_ownership"), Term::list(std::move(ownership))},
        {Term::atom("public_exports"), Term::list(std::move(pubExports))},
    });
    if (chunk.version >= 2) {
        auto& entries = std::get<Term::Map>(structural->value).pairs;
        entries.emplace_back(Term::atom("unit_id"),
                             Term::binary(chunk.metadata.unitId));
        entries.emplace_back(Term::atom("source_module"),
                             Term::binary(chunk.metadata.sourceModule));
        if (!chunk.metadata.package.id.empty())
            entries.emplace_back(Term::atom("package"),
                                 packageToTerm(chunk.metadata.package));
        if (!chunk.metadata.traitConformances.empty()) {
            std::vector<TermPtr> conformances;
            for (const auto& c : chunk.metadata.traitConformances)
                conformances.push_back(conformanceToTerm(c));
            entries.emplace_back(Term::atom("trait_conformances"),
                                 Term::list(std::move(conformances)));
        }
        if (!chunk.metadata.traitDefs.empty()) {
            std::vector<TermPtr> defs;
            for (const auto& td : chunk.metadata.traitDefs)
                defs.push_back(traitDefToTerm(td));
            entries.emplace_back(Term::atom("trait_defs"),
                                 Term::list(std::move(defs)));
        }
    }

    return Term::tuple({
        Term::atom("kexi"),
        Term::integer(chunk.version),
        typeIface,
        structural,
    });
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

namespace {
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
#ifdef __APPLE__
    CC_SHA256(data, static_cast<CC_LONG>(len), out);
#elif defined(__EMSCRIPTEN__)
    // KexI is not used in the wasm build; zero-fill as stub.
    (void)data; (void)len;
    std::memset(out, 0, 32);
#else
    SHA256(data, len, out);
#endif
}
} // namespace

auto computeInterfaceHash(const KexiChunk& chunk) -> Hash128 {
    auto term = chunkToTermWithoutHash(chunk);
    auto bytes = encodeEtf(term);

    uint8_t digest[32];
    sha256(bytes.data(), bytes.size(), digest);

    Hash128 h{};
    for (int i = 0; i < 16; i++) h[i] = digest[i];
    return h;
}

auto computeContentHash(const std::vector<uint8_t>& bytes) -> Hash128 {
    uint8_t digest[32];
    sha256(bytes.data(), bytes.size(), digest);
    Hash128 hash{};
    for (int i = 0; i < 16; i++) hash[i] = digest[i];
    return hash;
}

auto computeArtifactHash(const BeamFile& beam) -> Hash128 {
    auto implementation = beam;
    implementation.removeChunk(KEXI_CHUNK_ID);
    auto bytes = writeBeamFile(implementation);

    return computeContentHash(bytes);
}

auto serializeKexi(const KexiChunk& chunk) -> std::vector<uint8_t> {
    auto hashless = chunkToTermWithoutHash(chunk);
    auto bytes = encodeEtf(hashless);

    uint8_t digest[32];
    sha256(bytes.data(), bytes.size(), digest);
    Hash128 hash{};
    for (int i = 0; i < 16; i++) hash[i] = digest[i];

    // Now build the full term with the hashes included.
    auto term = chunkToTermWithoutHash(chunk);
    // v6+: {kexi, Version, InterfaceHash, ArtifactHash, SourceHash, BuildInfo,
    //        TypeInterface, Structural}. Older schemas omit build metadata.
    auto& inner = std::get<Term::Tuple>(term->value).elements;
    if (chunk.version >= 6) {
        auto buildInfo = Term::map({
            {Term::atom("intrinsic_abi"),
             Term::integer(chunk.intrinsicAbiVersion)},
            {Term::atom("backend_representation"),
             Term::integer(chunk.backendRepresentationVersion)},
        });
        inner.insert(inner.begin() + 2, std::move(buildInfo));
        inner.insert(inner.begin() + 2, hashToTerm(chunk.sourceHash));
        inner.insert(inner.begin() + 2, hashToTerm(chunk.artifactHash));
    }
    inner.insert(inner.begin() + 2, hashToTerm(hash));

    return encodeEtf(term);
}

auto deserializeKexi(const std::vector<uint8_t>& data) -> KexiChunk {
    auto term = decodeEtf(data);
    auto& top = term->asTuple();
    if (top.size() < 5 || !top[0]->isAtom("kexi"))
        throw EtfError("invalid KexI chunk: missing kexi tag");

    KexiChunk chunk;
    chunk.version = static_cast<int>(top[1]->asInt());
    if (chunk.version > KEXI_SCHEMA_VERSION)
        throw EtfError("unsupported KexI schema version " +
                        std::to_string(chunk.version) +
                        " (this compiler supports up to " +
                        std::to_string(KEXI_SCHEMA_VERSION) + ")");

    chunk.interfaceHash = termToHash(top[2]);

    size_t payloadIndex = 3;
    if (chunk.version >= 6) {
        if (top.size() < 8)
            throw EtfError("invalid KexI v6 chunk: missing build metadata");
        chunk.artifactHash = termToHash(top[3]);
        chunk.sourceHash = termToHash(top[4]);
        if (auto abi = top[5]->mapGet("intrinsic_abi"))
            chunk.intrinsicAbiVersion = static_cast<int>(abi->asInt());
        if (auto backend = top[5]->mapGet("backend_representation"))
            chunk.backendRepresentationVersion =
                static_cast<int>(backend->asInt());
        payloadIndex = 6;
    }

    // Type interface
    auto ti = top[payloadIndex];
    if (auto exports = ti->mapGet("exports"))
        for (const auto& e : exports->asList())
            chunk.typeInterface.exports.push_back(termToExport(e));
    if (auto constants = ti->mapGet("constants"))
        for (const auto& c : constants->asList())
            chunk.typeInterface.constants.push_back(termToConstant(c));
    if (auto types = ti->mapGet("types"))
        for (const auto& t : types->asList())
            chunk.typeInterface.types.push_back(termToTypeExport(t));
    if (auto methods = ti->mapGet("methods"))
        for (const auto& m : methods->asList())
            chunk.typeInterface.methods.push_back(termToMethod(m));

    // Structural metadata
    auto sm = top[payloadIndex + 1];
    if (auto m = sm->mapGet("module")) chunk.metadata.moduleAtom = m->asBinaryStr();
    if (auto u = sm->mapGet("unit_id")) chunk.metadata.unitId = u->asBinaryStr();
    if (auto s = sm->mapGet("source_module"))
        chunk.metadata.sourceModule = s->asBinaryStr();
    if (auto f = sm->mapGet("foul")) chunk.metadata.isFoul = f->isAtom("true");
    if (auto r = sm->mapGet("role"))
        chunk.metadata.role = r->isAtom("companion") ? KexiModuleRole::Companion
                                                      : KexiModuleRole::Entry;
    if (auto bp = sm->mapGet("entry_back_pointer"))
        chunk.metadata.entryBackPointer = bp->asBinaryStr();
    if (auto cs = sm->mapGet("companions"))
        for (const auto& c : cs->asList())
            chunk.metadata.companions.push_back(termToCompanion(c));
    if (auto rs = sm->mapGet("records"))
        for (const auto& r : rs->asList())
            chunk.metadata.records.push_back(termToRecord(r));
    if (auto as = sm->mapGet("adts"))
        for (const auto& a : as->asList())
            chunk.metadata.adts.push_back(termToADT(a));
    if (auto mo = sm->mapGet("method_ownership"))
        for (const auto& o : mo->asList())
            chunk.metadata.methodOwnership.push_back(termToMethodOwnership(o));
    if (auto pe = sm->mapGet("public_exports"))
        for (const auto& e : pe->asList())
            chunk.metadata.publicExports.push_back(e->asBinaryStr());
    if (auto package = sm->mapGet("package"))
        chunk.metadata.package = termToPackage(package);
    if (auto tc = sm->mapGet("trait_conformances"))
        for (const auto& c : tc->asList())
            chunk.metadata.traitConformances.push_back(termToConformance(c));
    if (auto td = sm->mapGet("trait_defs"))
        for (const auto& d : td->asList())
            chunk.metadata.traitDefs.push_back(termToTraitDef(d));

    // v1 compatibility: ownership was implicit in the entry back-pointer and
    // the Kex.<Module> BEAM naming convention.
    if (chunk.metadata.unitId.empty()) {
        chunk.metadata.unitId = chunk.metadata.role == KexiModuleRole::Companion
            ? chunk.metadata.entryBackPointer : chunk.metadata.moduleAtom;
    }
    if (chunk.metadata.sourceModule.empty() &&
        chunk.metadata.moduleAtom.rfind("Kex.", 0) == 0) {
        chunk.metadata.sourceModule = chunk.metadata.moduleAtom.substr(4);
    }

    return chunk;
}

} // namespace kex::beam

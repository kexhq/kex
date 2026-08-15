#include "../../common/version.hxx"
#include "../../beam/beam_file.hxx"
#include "../../beam/etf.hxx"
#include "../evaluator.hxx"

namespace kex::interpreter {

namespace {

// An Erlang external term as a Kex value. The KexI chunk is a plain tree of
// tuples, lists, atoms, integers and binaries, so it maps across directly and
// a Kex program can walk it with ordinary pattern matching.
auto termToValue(const beam::TermPtr& term) -> ValuePtr {
    if (!term) return Value::none();
    return std::visit([](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, beam::Term::Atom>) {
            // The KexI schema uses `true`/`false` as booleans, not atoms.
            if (node.name == "true") return Value::boolean(true);
            if (node.name == "false") return Value::boolean(false);
            return Value::atom(node.name);
        } else if constexpr (std::is_same_v<T, beam::Term::Int>) {
            return Value::integer(node.value);
        } else if constexpr (std::is_same_v<T, beam::Term::Bin>) {
            return Value::string(std::string(node.data.begin(), node.data.end()));
        } else if constexpr (std::is_same_v<T, beam::Term::Tuple>) {
            std::vector<ValuePtr> elements;
            elements.reserve(node.elements.size());
            for (const auto& element : node.elements)
                elements.push_back(termToValue(element));
            return Value::tuple(std::move(elements));
        } else if constexpr (std::is_same_v<T, beam::Term::List>) {
            std::vector<ValuePtr> elements;
            elements.reserve(node.elements.size());
            for (const auto& element : node.elements)
                elements.push_back(termToValue(element));
            return Value::list(std::move(elements));
        } else if constexpr (std::is_same_v<T, beam::Term::Map>) {
            MapValue map;
            for (const auto& [key, value] : node.pairs)
                map.entries.push_back({termToValue(key), termToValue(value)});
            auto out = std::make_shared<Value>();
            out->data = std::move(map);
            return out;
        } else {
            return Value::list({});
        }
    }, term->value);
}

} // namespace

auto Evaluator::registerKexBuiltins() -> void {
    auto makeVariant = [](const std::string& tag) -> ValuePtr {
        auto v = std::make_shared<Value>();
        v->data = VariantValue{tag, "", {}};
        return v;
    };

    auto featureTag = [](const ValuePtr& arg) -> std::string {
        if (auto* vv = std::get_if<VariantValue>(&arg->data)) return vv->tag;
        if (auto* mv = std::get_if<ModuleValue>(&arg->data)) return mv->name;
        return "";
    };

    static const std::vector<std::string> interpreterFeatures = {"FS"};

    auto hasFeature = [featureTag](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto tag = featureTag(args[0]);
        for (const auto& f : interpreterFeatures)
            if (f == tag) return Value::boolean(true);
        return Value::boolean(false);
    };

    auto listFeatures = [makeVariant](std::vector<ValuePtr>) -> ValuePtr {
        return Value::list({makeVariant("FS")});
    };

    // Kex.backend and Kex.Feature.* are source-owned. These implementations
    // are reachable only through the private Kex.Intrinsic.Kex boundary.
    // inspect(value) -> String — the pretty-printed representation, exposed as
    // an intrinsic (not only as a public native) so the prelude can declare it
    // once and BOTH backends resolve the same name. Previously `.inspect` was
    // interpreter-only and failed with "Undefined method: inspect" on BEAM.
    defineIntrinsic("Kex::inspect", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("()");
        auto rendered = args[0]->inspect();
        if (args.size() > 1) {
            const auto* colors = std::get_if<BoolValue>(&args[1]->data);
            if (colors && !colors->value) {
                std::string plain;
                for (std::size_t i = 0; i < rendered.size();) {
                    if (rendered[i] == '\x1b' && i + 1 < rendered.size() &&
                        rendered[i + 1] == '[') {
                        i += 2;
                        while (i < rendered.size() && rendered[i] != 'm') ++i;
                        if (i < rendered.size()) ++i;
                    } else {
                        plain.push_back(rendered[i++]);
                    }
                }
                rendered = std::move(plain);
            }
        }
        return Value::string(std::move(rendered));
    });

    defineIntrinsic("Kex::show", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        return Value::string(args[0]->toString());
    });

    // Generic runtime category reflection used by libraries that operate on
    // Any values. This deliberately reports language-level categories rather
    // than library-specific tags; consumers such as JSON remain source-owned.
    defineIntrinsic("Kex::kind", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::atom("none");
        const auto& data = args[0]->data;
        if (auto* variant = std::get_if<VariantValue>(&data))
            return Value::atom(variant->tag == "None" ? "none" : "variant");
        if (std::holds_alternative<BoolValue>(data)) return Value::atom("bool");
        if (std::holds_alternative<IntValue>(data) ||
            std::holds_alternative<BigIntValue>(data))
            return Value::atom("integer");
        if (std::holds_alternative<FloatValue>(data)) return Value::atom("float");
        if (std::holds_alternative<StringValue>(data)) return Value::atom("string");
        if (std::holds_alternative<ListValue>(data)) return Value::atom("list");
        if (std::holds_alternative<MapValue>(data)) return Value::atom("map");
        if (std::holds_alternative<CharValue>(data)) return Value::atom("char");
        if (std::holds_alternative<AtomValue>(data)) return Value::atom("atom");
        if (std::holds_alternative<TupleValue>(data)) return Value::atom("tuple");
        if (std::holds_alternative<RecordValue>(data)) return Value::atom("record");
        if (std::holds_alternative<UnitValue>(data)) return Value::atom("void");
        return Value::atom("other");
    });

    defineIntrinsic("Kex::backend", [makeVariant](std::vector<ValuePtr>) -> ValuePtr {
        return makeVariant("Interpreter");
    });

    // Mirrors kex_intrinsic_kex:version/0 — see src/common/version.hxx for
    // why both sides read from one place.
    defineIntrinsic("Kex::version", [](std::vector<ValuePtr>) -> ValuePtr {
        auto revision =
            *kGitRevision
                ? Value::variant("Just", "", {Value::string(kGitRevision)})
                : Value::variant("None", "", {});
        return Value::tuple({Value::integer(kVersionMajor),
                             Value::integer(kVersionMinor),
                             Value::integer(kVersionPatch), revision});
    });

    // The release channel, "" for a stable build. Separate from version/0
    // rather than a fifth tuple element: the tuple is a published shape
    // (`Kex.Kernel.VERSION.tuple`), and widening it would break every
    // destructuring of it.
    defineIntrinsic("Kex::versionPreRelease", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::string(kVersionPreRelease);
    });

    // Kex.Intrinsic.Interface.read — the KexI chunk of a compiled module,
    // decoded. Reading a BEAM chunk and decoding an Erlang external term are
    // both things the compiler already does natively, so this works on the
    // tree walker as well as on BEAM; a Kex program needed
    // `Erlang.Beam_lib.chunks` plus `Erlang.Erlang.binary_to_term` for it.
    defineIntrinsic("Interface::read", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* path = std::get_if<StringValue>(&args[0]->data);
        if (!path) return Value::none();
        try {
            auto beam = beam::readBeamFile(path->value);
            const auto* chunk = beam.findChunk("KexI");
            if (!chunk) return Value::none();
            return Value::just(termToValue(beam::decodeEtf(chunk->data)));
        } catch (const std::exception&) {
            // A missing file, a non-BEAM file, or a chunk this build cannot
            // decode all mean the same thing to the caller: no interface here.
            return Value::none();
        }
    });

    defineIntrinsic("Kex::featureHas?", hasFeature);
    defineIntrinsic("Kex::featureList", listFeatures);
}

} // namespace kex::interpreter

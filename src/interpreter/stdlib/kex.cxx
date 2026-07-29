#include "../evaluator.hxx"

namespace kex::interpreter {

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
        return Value::string(args[0]->inspect());
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

    defineIntrinsic("Kex::featureHas?", hasFeature);
    defineIntrinsic("Kex::featureList", listFeatures);
}

} // namespace kex::interpreter

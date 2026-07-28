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

    defineIntrinsic("Kex::backend", [makeVariant](std::vector<ValuePtr>) -> ValuePtr {
        return makeVariant("Interpreter");
    });

    defineIntrinsic("Kex::featureHas?", hasFeature);
    defineIntrinsic("Kex::featureList", listFeatures);
}

} // namespace kex::interpreter

#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerKexBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Kex", Value::module("Kex"));
    m_globalEnv->define("Kex::Feature", Value::module("Kex.Feature"));

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

    reg("Kex::backend", [makeVariant](std::vector<ValuePtr>) -> ValuePtr {
        return makeVariant("Interpreter");
    });

    reg("Kex.Feature::has?", hasFeature);
    reg("Kex.Feature::list", listFeatures);
    reg("featureHas?", hasFeature);
    reg("featureList", listFeatures);
}

} // namespace kex::interpreter

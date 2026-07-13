#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerHttpBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Http", Value::module("Http"));

    auto httpError = [](const std::string& kind, const std::string& message) -> ValuePtr {
        return Value::error(Value::record("HttpError", {
            {"kind", Value::string(kind)},
            {"message", Value::string(message)},
        }));
    };

    auto httpRequest = [this, httpError](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockHttp) {
            if (m_mockHttpResponses.empty())
                return httpError("mock", "no responses staged");
            auto resp = m_mockHttpResponses.front();
            m_mockHttpResponses.pop_front();
            return Value::ok(resp);
        }
        return httpError("runtime", "Http requires BEAM backend");
    };

    reg("Http::get", httpRequest);
    reg("Http::post", httpRequest);
    reg("Http::put", httpRequest);
    reg("Http::patch", httpRequest);
    reg("Http::delete", httpRequest);
    reg("Http::head", httpRequest);
    reg("Http::options", httpRequest);

    if (!m_globalEnv->has("Mock"))
        m_globalEnv->define("Mock", Value::module("Mock"));

    // Nested modules use a dotted canonical name ("Mock.Http") and `::`
    // only before the member. Keep the native bindings aligned with the
    // ModuleValue installed by the prelude declaration.
    m_globalEnv->define("Mock::Http", Value::module("Mock.Http"));

    reg("Mock.Http::start", [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockHttp = true;
        m_mockHttpResponses.clear();
        return Value::unit();
    });

    reg("Mock.Http::respond", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto status = args[0];
        auto body = args[1];
        ValuePtr headers;
        if (args.size() >= 3) {
            headers = args[2];
        } else {
            auto m = std::make_shared<Value>();
            m->data = MapValue{};
            headers = m;
        }
        auto resp = Value::record("HttpResponse", {
            {"status", status},
            {"body", body},
            {"headers", headers},
        });
        m_mockHttpResponses.push_back(resp);
        return Value::unit();
    });

    reg("Mock.Http::stop", [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockHttp = false;
        m_mockHttpResponses.clear();
        return Value::unit();
    });
}

} // namespace kex::interpreter

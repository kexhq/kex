#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerHttpBuiltins() -> void {
    auto httpError = [](const std::string& kind, const std::string& message) -> ValuePtr {
        auto kindVal = std::make_shared<Value>();
        kindVal->data = VariantValue{kind, "NetworkError", {}};
        return Value::error(Value::record("HttpError", {
            {"kind", kindVal},
            {"message", Value::string(message)},
        }));
    };

    auto httpRequest = [this, httpError](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockHttp) {
            if (m_mockHttpResponses.empty())
                return httpError("MockEmpty", "no responses staged");
            auto resp = m_mockHttpResponses.front();
            m_mockHttpResponses.pop_front();
            return Value::ok(resp);
        }
        return httpError("NotImplemented", "Http requires BEAM backend");
    };

    // Request operations are private runtime capabilities. The public
    // Http.* functions are loaded from http.kex and delegate here.
    for (const char* name : {
             "Http::get", "Http::post", "Http::put", "Http::patch",
             "Http::delete", "Http::head", "Http::options"}) {
        defineIntrinsic(name, httpRequest);
    }

    auto mockStart = [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockHttp = true;
        m_mockHttpResponses.clear();
        return Value::unit();
    };

    auto mockRespond = [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    };

    auto mockStop = [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockHttp = false;
        m_mockHttpResponses.clear();
        return Value::unit();
    };

    // Mock.Http's public functions live in http.kex. Only its private state
    // controls are registered here.
    defineIntrinsic("Http::mockStart", std::move(mockStart));
    defineIntrinsic("Http::mockRespond", std::move(mockRespond));
    defineIntrinsic("Http::mockStop", std::move(mockStop));
}

} // namespace kex::interpreter

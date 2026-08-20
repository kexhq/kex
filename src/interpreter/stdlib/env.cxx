#include "../evaluator.hxx"

#if defined(__APPLE__)
#include <crt_externs.h>
#define KEX_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define KEX_ENVIRON environ
#endif

namespace kex::interpreter {

// ENV — all-caps because it's a constant (same convention as the ALL_CAPS
// compile-time constants in `compiled do ... end` blocks). It's just an
// immutable Map<String, String> snapshot of the process environment taken
// once at startup — no special methods of its own; ENV.get(key[, default]),
// ENV.keys, ENV.has?(key), etc. all work via the generic Map builtins
// (see map.cxx) since ENV is a plain MapValue.
auto Evaluator::registerEnvBuiltins() -> void {
    rebuildEnvMap();

    // Mock.ENV — the overlay a test writes. ENV is a snapshot, so setting a
    // variable rebuilds it; that is also what makes `keys`, `count` and
    // `each` agree with `get` instead of only the lookup being mocked.
    // Test-only like every Mock.* intrinsic (issue #144).
    defineIntrinsic("Env::mockSet", [this](std::vector<ValuePtr> args) -> ValuePtr {
        requireMocksAllowed("Mock.ENV.set");
        if (args.size() >= 2) {
            auto* name = std::get_if<StringValue>(&args[0]->data);
            auto* value = std::get_if<StringValue>(&args[1]->data);
            if (name && value) {
                m_mockEnv[name->value] = value->value;
                m_mockEnvUnset.erase(name->value);
                rebuildEnvMap();
            }
        }
        return Value::unit();
    });

    defineIntrinsic("Env::mockUnset", [this](std::vector<ValuePtr> args) -> ValuePtr {
        requireMocksAllowed("Mock.ENV.unset");
        if (!args.empty())
            if (auto* name = std::get_if<StringValue>(&args[0]->data)) {
                m_mockEnv.erase(name->value);
                m_mockEnvUnset.insert(name->value);
                rebuildEnvMap();
            }
        return Value::unit();
    });

    defineIntrinsic("Env::mockClear", [this](std::vector<ValuePtr>) -> ValuePtr {
        requireMocksAllowed("Mock.ENV.clear");
        m_mockEnv.clear();
        m_mockEnvUnset.clear();
        rebuildEnvMap();
        return Value::unit();
    });
}

// The ENV snapshot: the real environment with the mock overlay applied.
auto Evaluator::rebuildEnvMap() -> void {
    std::vector<std::pair<ValuePtr, ValuePtr>> entries;
    for (char** e = KEX_ENVIRON; e && *e; e++) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        auto name = entry.substr(0, eq);
        if (m_mockEnvUnset.count(name) || m_mockEnv.count(name)) continue;
        entries.push_back({Value::string(name), Value::string(entry.substr(eq + 1))});
    }
    for (const auto& [name, value] : m_mockEnv)
        entries.push_back({Value::string(name), Value::string(value)});
    auto envMap = std::make_shared<Value>();
    envMap->data = MapValue{std::move(entries)};
    m_globalEnv->define("ENV", envMap);
}

} // namespace kex::interpreter

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
    std::vector<std::pair<ValuePtr, ValuePtr>> entries;
    for (char** e = KEX_ENVIRON; e && *e; e++) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        entries.push_back({Value::string(entry.substr(0, eq)), Value::string(entry.substr(eq + 1))});
    }
    auto envMap = std::make_shared<Value>();
    envMap->data = MapValue{std::move(entries)};
    m_globalEnv->define("ENV", envMap);
}

} // namespace kex::interpreter

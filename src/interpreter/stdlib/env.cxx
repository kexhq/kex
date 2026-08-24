#include "../evaluator.hxx"

#include <cstdlib>

#if defined(__APPLE__)
#include <crt_externs.h>
#define KEX_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define KEX_ENVIRON environ
#endif

namespace kex::interpreter {

// ENV — the capability declared in env.kex, backed by a snapshot of the
// process environment with the Mock.ENV overlay applied. The snapshot is
// rebuilt on every write so `ENV.get` and a child process never disagree.
auto Evaluator::registerEnvBuiltins() -> void {
    rebuildEnvMap();

    // The ENV capability's readers.
    //
    // The walker used to answer `ENV.get`, `ENV.keys` and the rest through
    // MAP dispatch: `ENV` was bound to a plain MapValue, and every one of
    // those names happens to exist on Map. That worked by coincidence — it
    // left `ENV.set`, which Map has no answer for, undefined here while it
    // worked on BEAM, and it meant the capability declared in env.kex was
    // never the thing `ENV` named. These implement the capability instead, so
    // both backends answer through the same declaration.
    const auto entries = [this]() -> std::vector<std::pair<ValuePtr, ValuePtr>> {
        if (!m_envMap) return {};
        if (auto* map = std::get_if<MapValue>(&m_envMap->data)) return map->entries;
        return {};
    };
    const auto lookup = [entries](const std::string& key) -> ValuePtr {
        for (const auto& [name, value] : entries())
            if (name && name->toString() == key) return value;
        return nullptr;
    };

    defineIntrinsic("Env::get", [lookup](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto found = lookup(args[0]->toString());
        return found ? Value::just(found) : Value::none();
    });
    defineIntrinsic("Env::getWithDefault", [lookup](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::string("");
        auto found = lookup(args[0]->toString());
        return found ? found : args[1];
    });
    defineIntrinsic("Env::has?", [lookup](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        return Value::boolean(lookup(args[0]->toString()) != nullptr);
    });
    defineIntrinsic("Env::keys", [entries](std::vector<ValuePtr>) -> ValuePtr {
        std::vector<ValuePtr> keys;
        for (const auto& [name, _] : entries()) keys.push_back(name);
        return Value::list(std::move(keys));
    });
    defineIntrinsic("Env::values", [entries](std::vector<ValuePtr>) -> ValuePtr {
        std::vector<ValuePtr> values;
        for (const auto& [_, value] : entries()) values.push_back(value);
        return Value::list(std::move(values));
    });
    defineIntrinsic("Env::count", [entries](std::vector<ValuePtr>) -> ValuePtr {
        return Value::integer(static_cast<int64_t>(entries().size()));
    });
    defineIntrinsic("Env::entries", [entries](std::vector<ValuePtr>) -> ValuePtr {
        std::vector<ValuePtr> pairs;
        for (const auto& [name, value] : entries())
            pairs.push_back(Value::tuple({name, value}));
        return Value::list(std::move(pairs));
    });
    // `each` takes the pair as two arguments, matching `maps:foreach/2` on
    // BEAM and the `{ |key, value| ... }` a caller writes.
    defineIntrinsic("Env::each", [entries](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* fn = std::get_if<FunctionValue>(&args[0]->data);
        if (!fn || !fn->native) return Value::unit();
        for (const auto& [name, value] : entries()) fn->native({name, value});
        return Value::unit();
    });

    // ENV.set(name, value) — set a variable in THIS process and in the
    // children it starts.
    //
    // `ENV` itself is an immutable snapshot, so the map is rebuilt after the
    // write; otherwise `ENV.get` would keep answering what the process
    // started with while a child saw something else.
    //
    // The reason this exists: a program that shells out sometimes has to
    // decide what the child sees. Tey knows which compiler it selected and
    // has to hand that to `Kex.AST`, which reads `$KEX` — without a setter
    // the only channel between them is whatever PATH happens to hold, and a
    // manifest was being parsed by an unrelated `kex` or by none at all.
    defineIntrinsic("Env::set", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* name = std::get_if<StringValue>(&args[0]->data);
        auto* value = std::get_if<StringValue>(&args[1]->data);
        if (!name || !value || name->value.empty()) return Value::unit();
        // An `=` in the NAME would write a variable nobody can read back.
        if (name->value.find('=') != std::string::npos) return Value::unit();
        setenv(name->value.c_str(), value->value.c_str(), 1);
        rebuildEnvMap();
        return Value::unit();
    });

    // ENV.unset(name) — remove a variable from this process and its children.
    defineIntrinsic("Env::unset", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        if (auto* name = std::get_if<StringValue>(&args[0]->data)) {
            if (name->value.empty()) return Value::unit();
            unsetenv(name->value.c_str());
            rebuildEnvMap();
        }
        return Value::unit();
    });

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

    // Mock.ENV.vars({name: value, ...}) -> Void — the whole overlay in one
    // call, matching how `Mock.Env { vars: ... }` is written (kexhq/kex#143).
    defineIntrinsic("Env::mockVars", [this](std::vector<ValuePtr> args) -> ValuePtr {
        requireMocksAllowed("Mock.ENV.vars");
        if (args.empty()) return Value::unit();
        if (auto* map = std::get_if<MapValue>(&args[0]->data))
            for (const auto& [key, value] : map->entries) {
                if (!key || !value) continue;
                m_mockEnv[key->toString()] = value->toString();
                m_mockEnvUnset.erase(key->toString());
            }
        rebuildEnvMap();
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
    auto snapshot = std::make_shared<Value>();
    snapshot->data = MapValue{std::move(entries)};
    m_envMap = std::move(snapshot);
}

} // namespace kex::interpreter

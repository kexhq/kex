#pragma once

#include "value.hxx"

namespace kex::interpreter {

struct ValueBuilder {
    using Value = ValuePtr;
    using Fields = std::vector<std::pair<std::string, Value>>;

    auto variant(std::string tag, std::string parent,
                 std::vector<Value> args) const -> Value {
        return interpreter::Value::variant(std::move(tag), std::move(parent),
                                           std::move(args));
    }
    auto record(std::string name, Fields fields) const -> Value {
        std::unordered_map<std::string, Value> mapped;
        for (auto& [key, value] : fields)
            mapped.emplace(std::move(key), std::move(value));
        return interpreter::Value::record("Kex.AST." + name,
                                          std::move(mapped));
    }
    auto list(std::vector<Value> values) const -> Value {
        return interpreter::Value::list(std::move(values));
    }
    auto string(std::string value) const -> Value {
        return interpreter::Value::string(std::move(value));
    }
    auto integer(int64_t value) const -> Value {
        return interpreter::Value::integer(value);
    }
    auto integer(mpz_class value) const -> Value {
        return interpreter::Value::bigInteger(std::move(value));
    }
    auto floating(double value) const -> Value {
        return interpreter::Value::floating(value);
    }
    auto atom(std::string value) const -> Value {
        return interpreter::Value::atom(std::move(value));
    }
    auto boolean(bool value) const -> Value {
        return interpreter::Value::boolean(value);
    }
    auto none() const -> Value { return interpreter::Value::none(); }
    auto just(Value value) const -> Value { return interpreter::Value::just(std::move(value)); }
};

} // namespace kex::interpreter

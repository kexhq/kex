#pragma once

#include "etf.hxx"

namespace kex::beam {

struct TermBuilder {
    using Value = TermPtr;
    using Fields = std::vector<std::pair<std::string, Value>>;

    auto variant(std::string tag, std::string,
                 std::vector<Value> args) const -> Value {
        if (args.empty())
            return Term::atom(std::move(tag));
        std::vector<Value> elements;
        elements.reserve(args.size() + 1);
        elements.push_back(Term::atom(std::move(tag)));
        for (auto& arg : args)
            elements.push_back(std::move(arg));
        return Term::tuple(std::move(elements));
    }
    auto record(std::string name, Fields fields) const -> Value {
        std::vector<Value> elements;
        elements.reserve(fields.size() + 1);
        elements.push_back(Term::atom("Kex.AST." + name));
        for (auto& [_, value] : fields)
            elements.push_back(std::move(value));
        return Term::tuple(std::move(elements));
    }
    auto list(std::vector<Value> values) const -> Value {
        return Term::list(std::move(values));
    }
    auto tuple(std::vector<Value> values) const -> Value {
        return Term::tuple(std::move(values));
    }
    auto string(std::string value) const -> Value {
        return Term::binary(value);
    }
    auto integer(int64_t value) const -> Value { return Term::integer(value); }
    auto integer(mpz_class value) const -> Value { return Term::integer(std::move(value)); }
    auto floating(double value) const -> Value { return Term::floating(value); }
    auto atom(std::string value) const -> Value { return Term::atom(std::move(value)); }
    auto boolean(bool value) const -> Value {
        return Term::atom(value ? "true" : "false");
    }
    auto none() const -> Value { return Term::atom("None"); }
    auto just(Value value) const -> Value {
        return Term::tuple({Term::atom("Just"), std::move(value)});
    }
};

} // namespace kex::beam

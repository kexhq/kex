#include "../evaluator.hxx"
#include <stdexcept>

namespace kex::interpreter {

// Just(x), Ok(x), Error(e) are used throughout examples/docs without a
// local `type Option<A> = Just(A) | None` / `type Result<A,E> = Ok(A) |
// Error(E)` declaration, i.e. they're meant to be prelude builtins, not
// user-declared. (None/Nothing need no registration — bare NoneValue
// already works, and zero-arg variants work via the UpperIdentifier
// atom fallback.) A user `type Result<A,E> = Ok(A) | Error(E)` etc. that
// re-declares these is harmless — execTopLevel's TypeDef handling just
// redefines them identically.
auto Evaluator::registerAdtConstructors() -> void {
    auto regCtor1 = [this](const std::string& name) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, [name](std::vector<ValuePtr> args) -> ValuePtr {
            return Value::record(name, {{"0", args.empty() ? Value::none() : args[0]}});
        }};
        m_globalEnv->define(name, val);
    };
    regCtor1("Just");
    regCtor1("Ok");
    regCtor1("Error");

    // Comparison — the result type of Comparable.compare.
    // Less/Equal/Greater are zero-arg constructors (atoms), same as None.
    auto regAtom = [this](const std::string& name) {
        auto val = std::make_shared<Value>();
        val->data = AtomValue{name};
        m_globalEnv->define(name, val);
    };
    regAtom("Less");
    regAtom("Equal");
    regAtom("Greater");

    // ok?/error? (Result<T, E> = Ok(T) | Error(E)) and some?/none?
    // (T? = Just(T) | None) — predicates over the same prelude ADTs, so
    // callers don't have to hand-write a `match` just to ask "did this
    // succeed". Each pair only accepts values from its own ADT family —
    // calling `.ok?` on a non-Result (a String, a Just(...), an unrelated
    // record, ...) throws rather than silently answering `false`, since
    // a wrong-type receiver is almost always a caller bug, not a "no".
    auto regResultPredicate = [this](const std::string& name, const std::string& tag) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, [name, tag](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.empty()) throw std::runtime_error(name + " expects a Result, got no argument");
            auto* rec = std::get_if<RecordValue>(&args[0]->data);
            if (!rec || (rec->typeName != "Ok" && rec->typeName != "Error")) {
                throw std::runtime_error(name + " expects a Result (Ok/Error), got " + args[0]->typeName());
            }
            return Value::boolean(rec->typeName == tag);
        }};
        m_globalEnv->define(name, val);
    };
    regResultPredicate("ok?", "Ok");
    regResultPredicate("error?", "Error");

    // `none?` checks bare NoneValue, not a "None" record — see the comment
    // above regCtor1 for why None needs no constructor of its own.
    auto regOptionPredicate = [this](const std::string& name, bool wantJust) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, [name, wantJust](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.empty()) throw std::runtime_error(name + " expects an Optional, got no argument");
            if (std::holds_alternative<NoneValue>(args[0]->data)) return Value::boolean(!wantJust);
            auto* rec = std::get_if<RecordValue>(&args[0]->data);
            if (!rec || rec->typeName != "Just") {
                throw std::runtime_error(name + " expects an Optional (Just/None), got " + args[0]->typeName());
            }
            return Value::boolean(wantJust);
        }};
        m_globalEnv->define(name, val);
    };
    regOptionPredicate("some?", true);
    regOptionPredicate("none?", false);

    // `or` — fallback extraction, shared by both prelude ADTs: unwraps
    // Ok(x)/Just(x) to x, or returns the given default for Error(_)/None.
    // Works on either family (unlike ok?/error?/some?/none?, which are
    // deliberately family-specific) since "give me the value or a
    // default" doesn't need to distinguish why there's no value.
    {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{"or", [](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.size() < 2) {
                throw std::runtime_error("or expects a receiver and a default value");
            }
            const auto& receiver = args[0];
            const auto& fallback = args[1];
            if (std::holds_alternative<NoneValue>(receiver->data)) return fallback;
            if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                if (rec->typeName == "Ok" || rec->typeName == "Just") {
                    auto it = rec->fields.find("0");
                    return it != rec->fields.end() ? it->second : Value::none();
                }
                if (rec->typeName == "Error") return fallback;
            }
            throw std::runtime_error(
                "or expects a Result (Ok/Error) or Optional (Just/None), got " + receiver->typeName());
        }};
        m_globalEnv->define("or", val);
    }
}

} // namespace kex::interpreter

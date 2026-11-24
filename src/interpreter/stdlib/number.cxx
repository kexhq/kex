#include "../evaluator.hxx"
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace kex::interpreter {

namespace {

// The value of an ASCII digit or letter digit ('a' and 'A' are 10), 99 otherwise.
auto digitValue(char c) -> int {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

// How many digits in `radix` `text` holds from `at`.
auto digitRun(const std::string& text, size_t at, int radix) -> size_t {
    size_t end = at;
    while (end < text.size() && digitValue(text[end]) < radix) {
        ++end;
    }
    return end - at;
}

// An optional "-" then digits in `radix`, and nothing else.
auto canonicalIntegerText(const std::string& text, int radix) -> bool {
    const size_t start = !text.empty() && text[0] == '-' ? 1 : 0;
    const size_t digits = digitRun(text, start, radix);
    return digits > 0 && start + digits == text.size();
}

// An optional "-", digits, then optionally ".digits" and "e" or "E" with an
// optional sign and digits, and nothing else.
auto canonicalFloatText(const std::string& text) -> bool {
    size_t at = !text.empty() && text[0] == '-' ? 1 : 0;
    size_t digits = digitRun(text, at, 10);
    if (digits == 0) {
        return false;
    }
    at += digits;
    if (at < text.size() && text[at] == '.') {
        digits = digitRun(text, at + 1, 10);
        if (digits == 0) {
            return false;
        }
        at += 1 + digits;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        ++at;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
            ++at;
        }
        digits = digitRun(text, at, 10);
        if (digits == 0) {
            return false;
        }
        at += digits;
    }
    return at == text.size();
}

} // namespace

auto Evaluator::registerNumberBuiltins() -> void {
    // asInteger/integerResult (value.hxx) treat IntValue and BigIntValue
    // uniformly, so modulo/abs work the same on a value that's overflowed
    // into the bignum representation as on a plain Int.
    defineIntrinsic("Integer.modulo", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) {
            return Value::integer(0);
        }
        auto a = asInteger(args[0]);
        auto b = asInteger(args[1]);
        if (a && b && *b != 0) {
            mpz_class result;
            mpz_mod(result.get_mpz_t(), a->get_mpz_t(), b->get_mpz_t());
            // `mpz_mod` is always NON-NEGATIVE, but Kex's modulo takes the
            // divisor's sign (number.kex: "the result has the same sign as
            // +n+, consistent with mathematical modulo"), which is what the
            // BEAM runtime computes. `7.modulo(-3)` was 1 here and -2 there.
            if (*b < 0 && result != 0) {
                result += *b;
            }
            return integerResult(result);
        }
        return Value::integer(0);
    });

    defineIntrinsic("Number.abs", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::integer(0);
        }
        if (auto i = asInteger(args[0])) {
            return integerResult(abs(*i));
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::floating(f->value < 0 ? -f->value : f->value);
        }
        return args[0];
    });

    defineIntrinsic("Number.sqrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::floating(0.0);
        }
        if (auto* i = std::get_if<IntValue>(&args[0]->data)) {
            return Value::floating(std::sqrt(static_cast<double>(i->value)));
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::floating(std::sqrt(f->value));
        }
        return Value::floating(0.0);
    });

    // n.times { |i| ... } — runs the block n times, passing the index 0..n-1.
    // A parameterless `do ... end` block (a `Block<Void>` overload) takes no
    // arguments — call it empty rather than routing an index into a callable
    // (e.g. a named zero-arg function) that cannot accept one. Lambdas
    // tolerate the extra argument either way; named callables do not.
    defineIntrinsic("Integer.times", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) {
            return args.empty() ? Value::none() : args[0];
        }
        auto* n = std::get_if<IntValue>(&args[0]->data);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!n || !fn || !fn->native) {
            return args[0];
        }
        if (fn->arity == 0) {
            for (int64_t i = 0; i < n->value; i++) {
                fn->native({});
            }
            return args[0];
        }
        for (int64_t i = 0; i < n->value; i++) {
            fn->native({Value::integer(i)});
        }
        return args[0];
    });

    defineIntrinsic("Number.floor", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::integer(0);
        }
        if (auto i = asInteger(args[0])) {
            return integerResult(*i);
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::integer(static_cast<int64_t>(std::floor(f->value)));
        }
        return Value::integer(0);
    });

    defineIntrinsic("Number.ceil", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::integer(0);
        }
        if (auto i = asInteger(args[0])) {
            return integerResult(*i);
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::integer(static_cast<int64_t>(std::ceil(f->value)));
        }
        return Value::integer(0);
    });

    // Truncate toward zero, a no-op on integers. Matches
    // kex_intrinsic_number:toInteger/1, which number.kex's `Float.toInteger`
    // has always called on BEAM — the interpreter simply never defined it.
    defineIntrinsic("Number.toInteger", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::integer(0);
        }
        if (auto i = asInteger(args[0])) {
            return integerResult(*i);
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::integer(static_cast<int64_t>(std::trunc(f->value)));
        }
        return Value::integer(0);
    });

    defineIntrinsic("Number.round", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::integer(0);
        }
        if (auto i = asInteger(args[0])) {
            return integerResult(*i);
        }
        if (auto* f = std::get_if<FloatValue>(&args[0]->data)) {
            return Value::integer(static_cast<int64_t>(std::round(f->value)));
        }
        return Value::integer(0);
    });

    defineModule("Float");
    defineIntrinsic("Float.formatFixed", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto places = args.size() > 1 ? asInteger(args[1]) : std::nullopt;
        if (args.size() != 2 || !std::holds_alternative<FloatValue>(args[0]->data) || !places ||
            *places < 0 || *places > 15) {
            throw RuntimeError("decimal places must be between 0 and 15", {});
        }
        const auto value = std::get<FloatValue>(args[0]->data).value;
        const auto count = static_cast<size_t>(places->get_ui());
        // Round the exact binary value, with ties away from zero, without
        // overflowing a Float or narrowing the integer part to int64_t.
        mpq_class exact(std::abs(value));
        mpz_class scale;
        mpz_ui_pow_ui(scale.get_mpz_t(), 10, count);
        mpz_class numerator = exact.get_num() * scale;
        const mpz_class denominator = exact.get_den();
        mpz_class rounded = numerator / denominator;
        if ((numerator % denominator) * 2 >= denominator) {
            ++rounded;
        }
        auto result = rounded.get_str();
        if (count > 0) {
            if (result.size() <= count) {
                result.insert(0, count + 1 - result.size(), '0');
            }
            result.insert(result.size() - count, 1, '.');
        }
        if (value < 0 && rounded != 0) {
            result.insert(0, 1, '-');
        }
        return Value::string(result);
    });

    // Integer.fromDigits(text, radix) and Float.fromDigits(text): the native
    // half of Integer/Float/Number.parse. They read canonical text only: a
    // "-" when negative, digits in the radix, and for a Float an optional
    // ".digits" and "e±digits" (src/stdlib/culture.kex's scanner spells what
    // it reads that way), answering None for anything else. So the parses
    // try them on the text itself first and only scan what they refuse:
    // canonical text reads the same in every culture with a "." decimal
    // point. A Float is also None when no Float holds it (1e999).
    defineIntrinsic("Float.fromDigits", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* text = args.size() == 1 ? std::get_if<StringValue>(&args[0]->data) : nullptr;
        if (!text || !canonicalFloatText(text->value)) {
            return Value::none();
        }
        const double value = std::strtod(text->value.c_str(), nullptr);
        return std::isfinite(value) ? Value::just(Value::floating(value)) : Value::none();
    });

    defineModule("Integer");
    defineIntrinsic("Integer.fromDigits", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* text = args.size() == 2 ? std::get_if<StringValue>(&args[0]->data) : nullptr;
        const auto radix = args.size() == 2 ? asInteger(args[1]) : std::nullopt;
        if (!text || !radix || *radix < 2 || *radix > 36 ||
            !canonicalIntegerText(text->value, static_cast<int>(radix->get_si()))) {
            return Value::none();
        }
        return Value::just(
            integerResult(mpz_class(text->value, static_cast<int>(radix->get_si()))));
    });

    defineModule("Number");
}

} // namespace kex::interpreter

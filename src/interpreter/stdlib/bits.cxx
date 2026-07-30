#include "../evaluator.hxx"
#include <stdexcept>

namespace kex::interpreter {

namespace {

// Every operation here works on the arbitrary-precision representation. GMP
// models a negative integer as infinite-precision two's complement, which is
// exactly what Erlang's band/bor/bxor/bnot/bsl/bsr do, so the walker and the
// BEAM backend agree on negatives and bignums without any special casing
// (`-1 band 255` is 255 on both; `bnot 5` is -6).
auto operand(const std::vector<ValuePtr>& args, size_t index, const char* what)
    -> mpz_class {
    if (index >= args.size())
        throw std::runtime_error(std::string(what) + " expects an Integer");
    auto value = asInteger(args[index]);
    if (!value)
        throw std::runtime_error(std::string(what) + " expects an Integer, got " +
                                 args[index]->typeName());
    return *value;
}

// Shift distances and bit indices address a position, so they must be
// non-negative and small enough to be a real position. Erlang raises on a
// negative bit index too (a negative `bsl` there flips direction, which
// silently turning `shiftLeft` into a shift right would only hide a bug).
auto position(const std::vector<ValuePtr>& args, size_t index, const char* what)
    -> unsigned long {
    auto value = operand(args, index, what);
    if (value < 0)
        throw std::runtime_error(std::string(what) +
                                 ": bit position cannot be negative");
    if (!value.fits_ulong_p())
        throw std::runtime_error(std::string(what) + ": bit position too large");
    return value.get_ui();
}

} // namespace

// Bits — bitwise operations on Integer. The typed surface lives in
// src/stdlib/bits.kex; kex_intrinsic_bits.erl is the BEAM twin.
auto Evaluator::registerBitsBuiltins() -> void {
    defineModule("Bits");

    defineIntrinsic("Bits::and", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_and(out.get_mpz_t(), operand(args, 0, "Bits.and").get_mpz_t(),
                operand(args, 1, "Bits.and").get_mpz_t());
        return integerResult(std::move(out));
    });
    defineIntrinsic("Bits::or", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_ior(out.get_mpz_t(), operand(args, 0, "Bits.or").get_mpz_t(),
                operand(args, 1, "Bits.or").get_mpz_t());
        return integerResult(std::move(out));
    });
    defineIntrinsic("Bits::xor", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_xor(out.get_mpz_t(), operand(args, 0, "Bits.xor").get_mpz_t(),
                operand(args, 1, "Bits.xor").get_mpz_t());
        return integerResult(std::move(out));
    });
    defineIntrinsic("Bits::not", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_com(out.get_mpz_t(), operand(args, 0, "Bits.not").get_mpz_t());
        return integerResult(std::move(out));
    });

    defineIntrinsic("Bits::shiftLeft", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_mul_2exp(out.get_mpz_t(), operand(args, 0, "Bits.shiftLeft").get_mpz_t(),
                     position(args, 1, "Bits.shiftLeft"));
        return integerResult(std::move(out));
    });
    // Arithmetic (sign-propagating) shift, matching Erlang's bsr: -8 >> 1 is
    // -4, not a huge positive number.
    defineIntrinsic("Bits::shiftRight", [](std::vector<ValuePtr> args) -> ValuePtr {
        mpz_class out;
        mpz_fdiv_q_2exp(out.get_mpz_t(), operand(args, 0, "Bits.shiftRight").get_mpz_t(),
                        position(args, 1, "Bits.shiftRight"));
        return integerResult(std::move(out));
    });

    defineIntrinsic("Bits::test?", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto value = operand(args, 0, "Bits.test?");
        return Value::boolean(
            mpz_tstbit(value.get_mpz_t(), position(args, 1, "Bits.test?")) != 0);
    });
    defineIntrinsic("Bits::set", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto out = operand(args, 0, "Bits.set");
        mpz_setbit(out.get_mpz_t(), position(args, 1, "Bits.set"));
        return integerResult(std::move(out));
    });
    defineIntrinsic("Bits::clear", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto out = operand(args, 0, "Bits.clear");
        mpz_clrbit(out.get_mpz_t(), position(args, 1, "Bits.clear"));
        return integerResult(std::move(out));
    });
    defineIntrinsic("Bits::toggle", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto out = operand(args, 0, "Bits.toggle");
        mpz_combit(out.get_mpz_t(), position(args, 1, "Bits.toggle"));
        return integerResult(std::move(out));
    });

    // A negative value has infinitely many set bits under two's complement, so
    // both of these are defined only for non-negative input rather than
    // returning something arbitrary.
    defineIntrinsic("Bits::count", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto value = operand(args, 0, "Bits.count");
        if (value < 0)
            throw std::runtime_error("Bits.count is undefined for a negative Integer");
        return Value::integer(static_cast<int64_t>(mpz_popcount(value.get_mpz_t())));
    });
    defineIntrinsic("Bits::width", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto value = operand(args, 0, "Bits.width");
        if (value < 0)
            throw std::runtime_error("Bits.width is undefined for a negative Integer");
        if (value == 0) return Value::integer(0);
        return Value::integer(
            static_cast<int64_t>(mpz_sizeinbase(value.get_mpz_t(), 2)));
    });
}

} // namespace kex::interpreter

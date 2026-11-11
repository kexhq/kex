#pragma once
// What a type written in a program says about a value's form, as the checker
// reads the spelling: the declaration it names, or the structural form it
// spells. Aliases are followed, type arguments dropped, and a trait or a type
// variable admits every value. It says nothing about how a backend represents
// a value; each backend turns it into its own run-time test. Kept apart from
// typechecker.hxx so lowering can read it without the checker.
#include <cstdint>
#include <string>

namespace kex::semantic {

enum class TypeForm {
    // Any value: `Any`, a trait, a type variable, or nothing written.
    Any,
    // A function taking `arity` arguments (`Env -> String`).
    Function,
    // A parameterless block (`Block<A>`).
    Block,
    // `[Char]`, which a String is too.
    CharList,
    List,
    Map,
    Tuple,
    // A named type: a primitive, a record, a sum type, an opaque or distinct
    // type.
    Named,
};

struct TypeTest {
    TypeForm form = TypeForm::Any;
    // Named: the type's name as declared, without its module (`Client`).
    std::string name;
    // Named: the type's identity, its module path included when a module
    // declares it (`Net.HTTP.Client`); otherwise `name`.
    std::string identity;
    // Function: the number of parameters.
    int64_t arity = 0;
};

} // namespace kex::semantic

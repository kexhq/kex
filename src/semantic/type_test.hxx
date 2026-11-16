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
    List,
    Map,
    Tuple,
    // A named type: a primitive, a record, a sum type, an opaque or distinct
    // type.
    Named,
};

struct TypeTest {
    TypeForm form = TypeForm::Any;
    // Named: the type a value is tested against, aliases followed and type
    // arguments dropped (`Client`), and its identity, its module path
    // included when a module declares it (`Net.HTTP.Client`).
    std::string name;
    std::string identity;
    // The type named as written, aliases NOT followed, type arguments dropped,
    // and its identity; empty for a structural spelling (`[A]`, `A -> B`).
    std::string written;
    std::string writtenIdentity;
    // Function: the number of parameters.
    int64_t arity = 0;
};

} // namespace kex::semantic

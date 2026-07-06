# Add `truthy?` prelude builtin

## Context

Kex's `if`/`while`/`&&`/`||` all use a truthiness rule (`Value::isTrue()` in
`src/interpreter/value.cxx:113`) where only `false`, `None`, and `()` are
falsy — everything else (`0`, `""`, `[]`, records, variants, ...) is truthy.
There's currently no way for user code to ask "is this value truthy?"
without writing an `if` themselves. This adds a prelude function that
exposes this check directly for any value.

## Approach

Add a native builtin `truthy?` predicate, following the exact pattern
already used for ADT-family-agnostic predicates in
`src/interpreter/stdlib/adt.cxx` (e.g. the `or` builtin at the end of
`registerAdtConstructors`). Unlike `ok?`/`error?`/`some?`/`none?`
(which are deliberately type-specific and throw on the wrong ADT), `truthy?`
accepts any value, since truthiness is defined for every value in the
language.

### Implementation

In `src/interpreter/stdlib/adt.cxx`, inside `Evaluator::registerAdtConstructors()`,
after the `or` block:

```cpp
{
    auto val = std::make_shared<Value>();
    val->data = FunctionValue{"truthy?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("truthy? expects a value");
        return Value::boolean(args[0]->isTrue());
    }};
    m_globalEnv->define("truthy?", val);
}
```

This reuses `Value::isTrue()` directly (declared in `value.hxx:157`,
defined in `value.cxx:113`), so `truthy?` stays perfectly in sync with
whatever `if`/`&&`/`||` consider truthy — no duplicated logic.

No changes needed elsewhere: `adt.cxx`'s `registerAdtConstructors()` is
already wired into evaluator setup, and `?`-suffixed identifiers are already
supported by the lexer/parser (used by `ok?`, `none?`, etc.).

## Verification

- `cmake --build build` to compile.
- Quick manual check via `./build/kex -i` or a scratch `.kex` file:
  ```
  IO.printLine(truthy?(false))   # false
  IO.printLine(truthy?(None))    # false
  IO.printLine(truthy?(()))      # false
  IO.printLine(truthy?(0))       # true
  IO.printLine(truthy?(""))      # true
  IO.printLine(truthy?([]))      # true
  ```
- Run `make test-all` to confirm no regressions (per project convention).

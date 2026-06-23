# Testing

## Standard Library Framework

Testing is a library, not a language feature. `describe`/`it`/`assert` are real, implemented as global stdlib functions (`registerTestBuiltins` in `src/interpreter/stdlib/test.cxx`) — the rest of this doc (`before`, `Mock.*`, `using Test`, the `kex test` subcommand) is still aspirational; see "Not Yet Implemented" below.

## Syntax

```kex
describe("loadConfig") do
  it("parses the config file") do
    let config = MyServer.loadConfig("config.toml")
    assert(config == Ok(Config { port: 8080, host: "localhost" }))
  end

  it("returns error for missing file") do
    let result = MyServer.loadConfig("missing.toml")
    assert(result.error?)
  end
end
```

Calls must be parenthesized — `describe("name") do`, not `describe "name" do` — the grammar has no support for a bare positional argument without parens (only a block or named-args-without-parens). `main do ... end` is not required either: a bare top-level expression like `describe(...) do ... end` is already wrapped into one by the parser.

## Key Components

- `describe(name) do ... end` — purely organizational: prints a header and runs its block. Can nest.
- `it(name) do ... end` — runs a test case. Any exception escaping the block — a failed `assert`, or an ordinary bug in the code under test — marks it failed and prints the message, without aborting the rest of the suite.
- `assert(value)` / `assert(value, message)` — throws if `value` is falsy (caught by the enclosing `it`; outside of `it`, it's just an ordinary uncaught error).

A summary line (`N passed, M failed`) prints once, at the end of the program, only if at least one `it` ran.

## Specs for example files: `<name>.spec.kex`

`<name>.spec.kex` is a spec for `<name>.kex` and doesn't need to redeclare its types/records/functions — running it auto-loads `<name>.kex`'s declarations (skipping its own `main` block, so its demo output doesn't run) into the same scope first. Lookup is same-directory by default, plus `examples/<name>.kex` as a fallback when the spec lives in a directory named `spec` (see `specBaseCandidates` in `src/main.cxx`) — which is how `spec/json_parser.spec.kex` tests `examples/json_parser.kex` without copying any of its code.

## Not Yet Implemented

- `before` — setup run before each test in a group
- `Mock.*` — mock implementations of foul modules (`Mock.File.expect(...)`, etc.)
- `using Test` / `using` blocks in general — currently a no-op everywhere in the interpreter
- `kex test` — a dedicated CLI subcommand for running specs (today, specs are just run like any other `.kex` file: `kex spec/foo.spec.kex`)

```kex
# Aspirational — none of this works yet:
using Test

describe MyServer do
  before do
    Mock.File.expect(:read, "config.toml", returns: Ok("port=8080"))
  end
end
```

# Testing

## Standard Library Framework

Testing is a library, not a language feature. `describe`, `it`, `before`, and
`after` are global stdlib functions; focused assertions live in `Assert`. Hooks are scoped to their
`describe`: setup runs outer-to-inner in declaration order, while teardown always
runs inner-to-outer in reverse declaration order. `Mock.FS` and `Mock.Http` are
also implemented; `using Test` and the `kex test` subcommand remain aspirational.

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
- `before { ... }` / `after { ... }` — per-test setup and guaranteed cleanup; `:each` is the optional default scope, while `before(:all)` / `after(:all)` run once per group.
- `assert(value)` / `assert(value, message)` — throws if `value` is falsy (caught by the enclosing `it`; outside of `it`, it's just an ordinary uncaught error).
- `Assert.equal`, `Assert.notEqual`, `Assert.truthy`, `Assert.falsy`, `Assert.some`, `Assert.none`, `Assert.ok`, and `Assert.error` — focused assertions with clearer failures.

A summary line (`N passed, M failed`) prints once, at the end of the program, only if at least one `it` ran.

## Specs for example files: `<name>.spec.kex`

`<name>.spec.kex` is a spec for `<name>.kex` and doesn't need to redeclare its types/records/functions — running it auto-loads `<name>.kex`'s declarations (skipping its own `main` block, so its demo output doesn't run) into the same scope first. Lookup is same-directory by default, plus `examples/<name>.kex` as a fallback when the spec lives in a directory named `spec` (see `specBaseCandidates` in `src/main.cxx`) — which is how `spec/json_parser.spec.kex` tests `examples/json_parser.kex` without copying any of its code.

## Not Yet Implemented

- `using Test` / `using` blocks in general — currently a no-op everywhere in the interpreter
- `kex test` — a dedicated CLI subcommand for running specs (today, specs are just run like any other `.kex` file: `kex spec/foo.spec.kex`)

```kex
# Aspirational dedicated import and richer expectation API:
using Test

describe MyServer do
  before do
    Mock.File.expect(:read, "config.toml", returns: Ok("port=8080"))
  end
end
```

# Testing

## Standard Library Framework

Testing is a library, not a language feature. `describe`, `it`, `before`, and
`after` are global stdlib functions; focused assertions live in `Assert`. Hooks are scoped to their
`describe`: setup runs outer-to-inner in declaration order, while teardown always
runs inner-to-outer in reverse declaration order. `Mock.FS` and `Mock.Http` are
also implemented; `using Test` and the `kex test` subcommand remain aspirational.

## Replacing the world: two options

The filesystem, the environment and the network are **capabilities** — modules
whose implementation can be replaced for a lexical region with `with`. That is
the option to reach for first:

```kex
with FS.File = Mock.Files { files: {"kex.toml": "name = \"demo\""} } do
  assert(loadConfig() == "demo")
end
```

`loadConfig` declares no dependency and threads no parameter; its
`FS.File.read` call is written exactly as it is in production. That is the
point — if faking the filesystem meant rewriting the call site, the code under
test would no longer be the code that ships.

A replacement is a value, so it holds no global state, needs no clearing,
cannot leak past its block, and two regions can use different ones at the same
time. The stdlib ships one for each capability so a test does not have to spell
out every member by hand:

| Capability | Stand-in | Callback |
| --- | --- | --- |
| `FS.File` | `Mock.Files { files: … }` | `onRead` |
| `ENV` | `Mock.Env { vars: … }` | `onGet` |
| `Http` | `Mock.Response { status:, body:, headers: }` | — |

The callback is for when a test wants a rule rather than a fixture — content
derived from the path, a variable that only exists under a prefix, a failure on
the third call. It governs the whole surface, not just the one method a test
happens to call: `onRead` answers `exists?`, `size`, `readLines` and `feed`
too. Returning `None` means "not there", so a callback can model absence as
well as presence, which matters because absence is an answer programs act on —
a default path, a disabled feature.

Writes are refused rather than silently accepted. A test that did not expect
one sees it fail instead of passing for the wrong reason.

What a value cannot do is **change**. A test that writes and then reads back,
or one whose `before`/`after` hooks observe an artifact across separate blocks,
still needs the stateful `Mock.*` functions below.

## Mocks are test-only

A mock lets one part of a program lie to another about the filesystem, the
environment, the network, or the console — and the lie is global
and invisible at the call that believes it: `FS.File.read(configPath)` reviews
as safe no matter who mocked that path. So `Mock.*` is refused unless the run
is a test:

| Where | Mocks |
| --- | --- |
| `kex foo.spec.kex`, `kex -R foo.spec.kex` | allowed — the entry file is a spec |
| `kex -i` / `kex -R` REPL, the browser REPL | allowed — nothing else is present to deceive |
| `kex --allow-mocks foo.kex` | allowed — explicitly asked for |
| anything else, including a compiled `.beam` run straight from `erl` | **denied**, with an error naming the call |

Both backends deny with the same message, and the grant is made by whatever
starts the program, never baked into the artifact — so a compiled program
cannot be hijacked by a dependency that calls `Mock.ENV.set` at load time.

`Mock` is also no longer part of the automatic prelude. Qualified use
(`Mock.FS.File(...)`) loads it on demand, like any other opt-in stdlib module.

The gate covers the mock *functions*, which mutate global state. The stand-ins
above are ordinary records and reach no intrinsic, so they need no grant — a
program using `with FS.File = Mock.Files { … }` runs anywhere, which is what
makes them usable in an example as well as a spec.

There is deliberately no `Mock.System`. Faking the reported OS only exercises a
program's branching, not the platform behaviour behind it: the file semantics,
path rules and process handling that actually differ are unaffected by the
atom. Testing those means running on the platform, which CI does across macOS,
Ubuntu and Alpine.

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
    Mock.FS.File("config.toml", "port=8080")
  end
end
```

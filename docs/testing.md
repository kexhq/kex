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

## Machine-readable results: `--test-json`, `--test-list`, `--test-only`

The ✓/✗ prose is for people. A tool — an editor's test tree, a CI reporter —
needs the same run said in a form it can act on, and needs to be able to ask
for one case rather than a file, so three flags do that (kexhq/kex#199). They
work on both backends, and both emit byte-identical records for the same file:
`make spec-test-json` diffs them against a golden to keep it that way.

`--test-json` replaces the prose with one JSON object per line on stdout:

```console
$ kex --test-json spec/arithmetic.spec.kex
{"kexTest":"case","path":["arithmetic","adds"],"name":"adds","status":"passed","durationMs":0.181,"file":"spec/arithmetic.spec.kex","line":2,"column":3}
{"kexTest":"case","path":["arithmetic","divides"],"name":"divides","status":"failed","durationMs":0.237,"file":"spec/arithmetic.spec.kex","line":6,"column":3,"failure":{"message":"assertion failed: expected 3, got 2","file":"spec/arithmetic.spec.kex","line":7,"column":5}}
{"kexTest":"summary","passed":1,"failed":1}
```

A spec prints its own output to the same stream, so a line is a record only if
it parses as JSON **and** carries `kexTest`; anything else is the program
talking. `status` is `passed`, `failed`, or `skipped` (an `it` with no block).
Every location is a `file`/`line`/`column` triple, omitted entirely when
unknown — a case's location is its `it`, and a failure's is the `assert` that
raised it, or the `it` again when the failure came from somewhere with no
`assert` of its own (an `Assert.*` helper, or an ordinary bug in the code under
test). Fields may be added; a reader should ignore what it does not know.

`--test-list` reports the same `file`/`line` for every `describe` and `it`
without running a single case body:

```console
$ kex --test-list spec/arithmetic.spec.kex
{"kexTest":"item","kind":"describe","path":["arithmetic"],"name":"arithmetic","file":"spec/arithmetic.spec.kex","line":1,"column":1}
{"kexTest":"item","kind":"it","path":["arithmetic","adds"],"name":"adds","file":"spec/arithmetic.spec.kex","line":2,"column":3}
```

It is a real execution of the file's top level — the `describe` blocks run,
because they are what registers the cases — with every `it` body skipped. A
spec that generates its cases in a loop is therefore listed exactly as it will
run, which a parser of the source could not promise.

`--test-only <name>` runs one case, and is repeatable. The name is the full
path, joined with ` > `, or an ancestor `describe`'s path, or a bare label:

```console
$ kex --test-only "arithmetic > adds" spec/arithmetic.spec.kex
$ kex --test-only "arithmetic" spec/arithmetic.spec.kex     # the whole group
$ kex --test-only "adds" spec/arithmetic.spec.kex           # by label alone
```

### Through Tey

`tey test` passes all three through under shorter names, and takes the spec
files to run as arguments rather than always globbing `spec/`:

```console
$ tey test                                   # the whole suite, as prose
$ tey test spec/greet.spec.kex               # one file
$ tey test spec/greet.spec.kex --list        # its cases, as JSON, none run
$ tey test spec/greet.spec.kex --json --only "Greet > greets by name"
```

`--only` takes ONE name (Tey's parser keeps one value per option); the
compiler's `--test-only` repeats. `--list` wins over `--json`, since discovery
is already a JSON mode and it is the one that runs nothing. In either
machine-readable mode Tey prints nothing of its own — not even `No specs found`
— so what reaches stdout is records and whatever the spec itself printed.

A tool should prefer `tey test` in a package: the source roots a package's
specs need (its own `src/`, and every locked dependency's) are Tey's to know,
and `kex spec/greet.spec.kex` on its own cannot compile them. Outside a package
there is nothing for Tey to know and the compiler's own flags are one process
fewer.

The VS Code extension does exactly that (see
`editors/vscode/src/test-runner.ts` for the choice, and `test-explorer.ts` for
the tree): discovery draws the tree, `--only` is the per-case ▶, and the JSON
records are what the results and inline failure markers are read from.

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

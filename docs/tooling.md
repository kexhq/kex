# Tooling

## REPL

Built-in interactive REPL, foul by default. `kex` with no file launches the
BEAM-backed REPL; `kex -i` launches the tree-walk one:

```
$ kex

Kex Interactive 0.3.0 (beam) — press Ctrl+C to exit (type /help ENTER for commands)

kex> 1 + 2
=> 3 : Int

kex> let name = "John"
=> "John" : String

kex> name.count
=> 4 : Int

kex> name.
# tab completion shows: .count .upperCase .lowerCase .split .trim .reverse ...
```

### Features

- **Type display** — shows the type of every result
- **Tab completion** — UFCS-powered (type `.` to see all available functions)
- **Multi-line** — detects `do` without `end`, continues on next line
- **History** — arrow keys, searchable
- **Module loading** — load project files into the session

### Commands

REPL commands use a `/` prefix (use `/help` for the full list):

```
kex> /help                # show all commands (also: /h)
kex> /clear               # clear the terminal screen
kex> /load myfile.kex     # load a module from file
kex> /unload MyModule     # unload a previously loaded module
kex> /reload              # reload all loaded modules
kex> /reset               # clear all bindings
kex> /with FS.File = fake # replace a capability for the rest of the session
kex> /with                # list the session's replaced capabilities
kex> /without FS.File     # stop replacing it
kex> /set <opt>           # enable a feature
kex> /unset <opt>         # disable a feature
kex> /complete prefix     # show completions for a prefix
kex> /exit                # exit (also: /quit, /q, Ctrl+C)
```

## File Extension

`.kex`

## CLI

```
kex <file.kex>              # type-check and run on the BEAM (default)
kex <file.kex> --no-check   # skip type checking and run directly
kex                         # start the interactive BEAM REPL (no file)
kex -c <file.kex>           # compile to BEAM via Core Erlang
kex -R <file.kex>           # run with the tree-walk interpreter
kex -i                      # start the interactive tree-walk REPL
kex -e <code>               # enter <code> in the REPL before its first prompt
kex -C <file.kex>           # run semantic analysis only
kex -E <file.kex>           # emit Core Erlang (.core) without invoking erlc
kex --test-json <spec.kex>  # report describe/it as JSON records, with locations
kex --test-list <spec.kex>  # list the cases as JSON, running none of them
kex --test-only <name> ...  # run one case, or one describe (repeatable)
kex --sname a --cookie secret <file.kex>
                            # run as the distributed node a@<host>
kex --name a@host.example.com --cookie secret <file.kex>
                            # the same, with a fully-qualified host name
```

`-e`/`--eval` boots a REPL session: each `-e` is entered as if typed, in
order, before the first prompt, so its bindings, definitions and spawned
processes are there when the session starts. A failing line ends the session
with status 1 rather than leaving it half booted. It takes no file; add `-i`
for the tree-walk REPL:

```
kex -e 'using App.Console' -e 'let store = App.Console.boot()'
```

`/with` keeps a capability replaced for the rest of the session, as if every
later input were inside `with Capability = value do ... end`. The value is
evaluated once and checked against the capability when it is set. It boots
from `-e` like any other line, so an app console can hand every query its
connection:

```
kex -e 'let conn = App.Console.boot()' -e '/with Harmat.Connection = conn'
```

`kex` gates on type checking by default. Use `--no-check` to skip the
type-checker (useful when iterating on code that type-checks correctly at
runtime but has incomplete annotations).

`--sname`, `--name` and `--cookie` start the BEAM VM as a named node, so it
can reach other nodes and they can reach it. See "Clustering" in
[concurrency.md](concurrency.md).

The three `--test-*` flags are for tools reading a spec run rather than people:
one JSON record per case, carrying the file and line of the `it` and of the
failure. `docs/testing.md` documents the record shape; the VS Code test
explorer is built on them.

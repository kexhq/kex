# Tooling

## REPL

Built-in interactive REPL, foul by default. Both `kex -i` and `kex -R` with no
file launch the same BEAM-backed REPL:

```
$ kex -i

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
kex> /load myfile.kex     # load a module from file
kex> /unload MyModule     # unload a previously loaded module
kex> /reload              # reload all loaded modules
kex> /reset               # clear all bindings
kex> /set <opt>           # enable a feature
kex> /unset <opt>         # disable a feature
kex> /complete prefix     # show completions for a prefix
kex> /exit                # exit (also: /quit, /q, Ctrl+C)
```

## File Extension

`.kex`

## CLI

```
kex <file.kex>              # type-check and run (default)
kex <file.kex> --no-check   # skip type checking and run directly
kex -i                      # start the interactive BEAM REPL (same as `kex -R` with no file)
kex -c <file.kex>           # compile to BEAM via Core Erlang
kex -R <file.kex>           # run on BEAM
kex -C <file.kex>           # run semantic analysis only
kex -e <file.kex>           # emit Core Erlang (.core) without invoking erlc
```

`kex` gates on type checking by default. Use `--no-check` to skip the
type-checker (useful when iterating on code that type-checks correctly at
runtime but has incomplete annotations).

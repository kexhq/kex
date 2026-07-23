# Tooling

## REPL

Built-in interactive REPL (`kex -i` or `kex -R` with no file), foul by default:

```
$ kex -i

Kex Interactive 0.2.0 — press Ctrl+C to exit (type /help ENTER for commands)

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
kex> /load myfile.kex     # load a module from file
kex> /unload MyModule     # unload a previously loaded module
kex> /reload              # reload all loaded modules
kex> /reset               # clear all bindings
kex> /complete prefix     # show completions for a prefix
kex> /exit                # exit (also: Ctrl+C)
```

## File Extension

`.kex`

## CLI

```
kex <file.kex>              # type-check and run (default)
kex <file.kex> --no-check   # skip type checking and run directly
kex -i                      # start interactive REPL (or `kex -R` for BEAM REPL)
kex -c <file.kex>           # compile to BEAM via Core Erlang
kex -R <file.kex>           # run on BEAM
kex -C <file.kex>           # run semantic analysis only
kex -e <file.kex>           # emit Core Erlang (.core) without invoking erlc
```

`kex` gates on type checking by default. Use `--no-check` to skip the
type-checker (useful when iterating on code that type-checks correctly at
runtime but has incomplete annotations).

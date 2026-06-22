# Tooling

## REPL

Built-in interactive REPL, foul by default:

```
$ kex

kex> 1 + 2
=> 3 : Int

kex> let name = "John"
=> "John" : String

kex> name.length
=> 4 : Int

kex> name.
# tab completion shows: .length .upcase .downcase .split .trim .reverse ...
```

### Features

- **Type display** — shows the type of every result
- **Tab completion** — UFCS-powered (type `.` to see all available functions)
- **Multi-line** — detects `do` without `end`, continues on next line
- **History** — arrow keys, searchable
- **Module loading** — load project files into the session

### Commands

```
kex> :load MyModule       # load a module
kex> :type expr           # show type without evaluating
kex> :doc String.split    # show documentation
kex> :pure                # enter pure mode (no foul allowed)
kex> :foul                # back to foul mode
```

### Pure Mode

```
kex> :pure
kex(pure)> IO.read("x")
=> error: IO.read is foul, not available in pure mode

kex(pure)> 1 + 2
=> 3 : Int

kex(pure)> :foul
kex>
```

## File Extension

`.kex`

## CLI

```
kex <file.kex>       # compile and run
kex build            # compile to WASM
kex test             # run tests
kex repl             # start REPL (or just `kex` with no args)
```

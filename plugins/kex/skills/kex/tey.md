# Packages: `tey`

`kex` compiles and runs files. `tey` is the tool around it: it creates a
package, fetches its dependencies, builds, runs and tests it, and installs the
Kex toolchain itself. A program of more than a few files should be a package.
`tey help` lists every command.

## Starting one

```text
tey new shop --app        an application: has an entrypoint, `tey run` works
tey new textkit --lib     a library: modules for other packages to use
tey init                  the same, in the directory you are already in
```

`tey new shop --app` creates:

```text
shop/
  package.kex             the manifest
  src/main.kex            the entrypoint
  spec/                   specs: spec/*.spec.kex
  README.md  .gitignore  .gitattributes
  AGENTS.md  CLAUDE.md  .claude/skills/kex/SKILL.md
```

The last line is for AI agents: three small files that send an agent to
`kex --skill` before it writes any Kex. They hold no copy of the guide, so
they do not go stale. `tey new shop --app --skip-agent` leaves them out.

The entrypoint is an ordinary program:

```kex main.kex
main(args) do
  IO.printLine("Hello from shop!")
end
```

## The manifest

`package.kex` is Kex, checked like any other file:

```kex package.kex
package "shop" do
  version("0.1.0")
  description("A Kex package")
  kex(">= 0.3.0")

  entrypoint("src/main.kex")

  target("shop", entrypoint: "src/main.kex")
end
```

- `kex(">= 0.3.0")` is the compiler version the package needs.
- `entrypoint` is what `tey run` runs; a library has none.
- `target("shop", ...)` names a program `tey build` produces in `ebin/`.
- `command(...)` adds a command of your own; see below.

## The package's own commands

A task the project needs, such as seeding a database, a benchmark, a
release step or a code generator, is a command in `package.kex`, not a
Makefile, a shell script to remember, or a note in the README:

```text
command("seed", run: "script/seed.kex", description: "Fill the database with sample data")
command("where", run: "pwd", description: "Print the package root")
```

- `tey seed a b` runs it, from anywhere inside the package, with `a b` as the
  program's `main(args)`.
- A `run:` whose first word ends in `.kex` is a Kex program, given by its path
  from the package root. Anything else is a shell command line, run from the
  package root.
- `tey help` lists the commands under **Package commands**, and editors that
  ask tey what a project can do show them as tasks.
- The built-in names (`build`, `test`, `run`, ...) cannot be redeclared, so
  `tey test` means the same thing in every package.
- A command script is an ordinary program: give it options with
  `OptionParser`, as below.

## A command-line program

An application that reads options or commands uses `OptionParser`. Do not
walk `args` by hand: the parser declares the interface once, prints `--help`
from it, rejects a bad value with a message, and dispatches to the command.

```kex
using OptionParser

# The whole interface, declared once. Each option is a name, a short letter
# (or None), a help line, a default (or None), and whether it is required.
let CLI = OptionParser.define("notes", "Keep short notes in a file")
  .string("file", Just('f'), "Where the notes are kept", Just("notes.txt"), false)
  .integer("limit", Just('n'), "How many notes to show", Just("10"), false)
  .flag("verbose", Just('v'), "Say what is happening")
  .command("add", "<text>", "Add a note", ~addNote)
  .command("list", "Show the newest notes", ~listNotes)

# A command answers the exit code.
foul addNote(options: ParsedOptions) -> Integer do
  let text = options.arguments.join(" ")
  return 1 if text.empty?
  IO.printLine("added to ${options.value("file", "")}: ${text}")
  0
end

foul listNotes(options: ParsedOptions) -> Integer do
  let limit = options.integerValue("limit").or(10)
  IO.printLine("showing ${limit} from ${options.value("file", "")}")
  IO.printLine("(verbose)") if options.flagEnabled?("verbose")
  0
end

# `run` parses, prints help or the error, and calls the command.
main(args) do
  System.exit(CLI.run(args))
end
```

- An option is `string`, `integer` or `flag`: a long name, a short letter or
  `None`, a help line, then for `string` and `integer` a default (`Just("10")`
  or `None`) and whether it is required.
- A command is `command("list", "description", ~handler)`, or
  `command("add", "<text>", "description", ~handler)` when the help line should
  show what it takes. The handler gets the parsed options, with the command's
  own words removed, and answers the exit code.
- Read values with `options.value("file", "")`, `options.integerValue("limit")`
  and `options.flagEnabled?("verbose")`; the words that were not options are
  `options.arguments`.
- `CLI.run(args)` parses and dispatches. For a program without commands, use
  `CLI.parse(args)` instead, which answers the options or an error to show
  with `OptionParser.errorMessage(error)` and `CLI.help`.
- `kex -s OptionParser` and `kex -s OptionConfig` list the rest.

## Where files go

`src/` is the root that modules are found from, by the rule every module
follows: `module Shop.Cart` is the file `src/shop/cart.kex`, and any file in
the package reaches it with `using Shop.Cart`.

```text
src/main.kex              using Shop.Cart
src/shop/cart.kex         module Shop.Cart
spec/cart.spec.kex        using Shop.Cart, then describe("Cart") do ...
```

Specs go directly in `spec/` and are named `<name>.spec.kex`. `tey test` does
not look in directories below it.

## Working in a package

```text
tey run                   run the entrypoint
tey run a b               ...with arguments, which arrive in main(args)
tey test                  run every spec/*.spec.kex
tey test spec/cart.spec.kex
tey test --only "Cart > counts what was added"
tey build                 compile the package and its targets into ebin/
tey install               fetch dependencies and write tey.lock
```

- `tey run` and `tey test` use the BEAM. Add `--interpret` to use the built-in
  interpreter instead, which needs no Erlang: `tey test --interpret`.
- Plain `kex` still works on one file, but it does not know about `src/`.
  Tell it: `kex -C --source-root src src/main.kex`.
- `tey.lock` records the exact versions resolved. Keep it in version control.
  `ebin/` is build output and is ignored.

## Dependencies

A dependency is a Git repository holding a package, named in the manifest:

```text
tey add textkit --git https://github.com/you/textkit
```

puts a line like this in `package.kex` and updates `tey.lock`:

```text
tey("textkit", git: "https://github.com/you/textkit", tag: "~> 0.1")
```

The tag is the release: `~> 0.1` takes the newest `0.1.x` among the
repository's tags. `--branch` and `--ref` pin a branch or a commit instead.
After `tey install`, the dependency's modules are used like your own:
`using Textkit`. `tey update` re-resolves, and `tey list` shows what is locked.

## The toolchain

```text
tey kex                   Kex versions, newest first, and which is selected
tey kex install latest    install a Kex toolchain
tey kex use <version>     select one
tey kex which             the compiler the `kex` command runs
tey upgrade               upgrade tey itself
```

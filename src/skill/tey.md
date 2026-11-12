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
```

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
- `command("bench", run: "script/bench.kex", description: "...")` adds a
  command of your own, run as `tey bench`.

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

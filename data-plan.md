# Move `Set`/`UnorderedSet` under a `Data` module, and add `Stack` and `Queue`

## Context

`src/stdlib/set.kex` sits at the stdlib root with no module header, so `Set` and
`UnorderedSet` are bare global names that every program has to route around even
though the file is opt-in (`prelude.kex` deliberately excludes it). There is no
namespace for "the container types that are not `List`/`Map`", so `Stack` and
`Queue` — the two obvious next collections — would each land as another bare
global.

The outcome: a `Data` namespace holding all four container types, imported one
at a time (`using Data.Set`, `using Data.Stack`, `using Data.Queue`), with
`Data.Set`, `Data.UnorderedSet`, `Data.Stack` and `Data.Queue` as the type names
and BEAM tags. This is a breaking change to `Set`'s spelling; that is fine
pre-stability, and the only in-repo callers are one spec and one docs section.

## Layout

```
src/stdlib/data/set.kex      module Data   record Set<A>, UnorderedSet<A>
src/stdlib/data/stack.kex    module Data   record Stack<A>
src/stdlib/data/queue.kex    module Data   record Queue<A>
```

Every file's header is `module Data` — **not** `module Data.Set`. This is the
one non-obvious point and it is deliberate:

- A file-level module header qualifies the records under it. `module Net` in
  `src/stdlib/net.kex` is why `record Port` is tagged `{'Net.Port', …}` (see
  `runtime/src/kex_intrinsic_nettcp.erl:5`). A header of `module Data.Set` would
  therefore make the type `Data.Set.Set`.
- `using Data.Set` still resolves: `kex::module::Resolver::resolve`
  (`src/module/resolver.cxx`) maps the name to the path `data/set.kex`
  regardless of the header, and `ResolvePass::resolveUsing`
  (`src/semantic/resolve_pass.cxx:267`) then only needs a module *named*
  `Data.Set` to exist in the loaded file — which the nested `module Set do …
  end` constructor block (already in `set.kex:63`) provides once it is inside
  `module Data`. `Stack` and `Queue` get the same nested constructor blocks.
- Nothing validates header against path; there is no such diagnostic in
  `src/semantic/` or `src/module/`.

Consequences to accept and state in the file header comments:

- Bare `using Data` is **not** offered — there is no `data.kex` for the resolver
  to find. This matches `Units.SI`, `Net.HTTP` and `Control.Retry`, which are
  likewise only reachable by their full path.
- These files leave the prebuilt stdlib artifact. `standardLibraryArtifactSourceFiles`
  (`src/common/prelude_loader.hxx:226`) only takes opt-in sources whose parent is
  the stdlib root, so `Data.*` becomes a source-loaded dependency like `Net.*`.
  That is the mechanism behind the checker work below.
- Three files declaring the same `module Data` is new. A spec that imports two of
  them in one program (`using Data.Set` + `using Data.Stack`) is required, not
  optional.

## The checker half (kexhq/kex#229)

Today `set.kex` has no module header, so its `make` blocks are collected into the
synthetic `Prelude` module and published as receiver functions — which is the
only reason `mySet.map { … }` and `.count` type correctly. Under `module Data`
they stop being prelude receivers, and #229's failure mode applies: an unknown
receiver type lets `map`/`filter`/`count` bind to the prelude's `List` overloads
and lower to the wrong BEAM function. Two halves, both needed:

**1. Publish the make methods.** For source-loaded opt-in modules,
`sourceSemanticInterfaces(standardLibrarySourceFiles(), false, true)`
(`src/common/prelude_interfaces.hxx:919`) is what fills
`ImportedInterfaces::receiverFunctions`, and with `directBackendOwnership = true`
it publishes **only annotated** make members (`prelude_interfaces.hxx:482-485`).
`set.kex` is already ~90% annotated (`contains? :> A -> Bool`, `count :> Integer`,
`map :> (A -> B) -> Set<B>`, …). The gaps are `reduce`, `identity`, `combine` and
the `+`/`-` operator overloads. Give every make member in all three new files a
`:>` annotation — operator annotations are grammatical (`grammar.ebnf:207`), and
the overloaded `+`/`-` need one annotation per overload (verify the parser accepts
that; if it does not, file it and annotate the single dominant overload).

**2. Resolve a bare module segment against an imported path.** `using Data.Set`
registers the module under the key `Data.Set`, but the call site writes
`Set.from([…])`, and that key lookup (`src/semantic/typechecker.cxx:4188-4214`,
mirrored in `src/semantic/db.cxx:386`) misses, so the result types as `Unknown`.
Teach `ResolvePass::resolveUsing` to record, for each `using M`, an alias from
`M`'s last segment to `M`, plus one for each immediate child module `M.C` under
the bare name `C`; have the checker's module-path candidate walk consult those
aliases before giving up. Two imported modules aliasing the same bare segment is
an ambiguity error, matching the existing `ImportOrigin` conflict handling in
`resolveUsing`. This also fixes `using Net.HTTP` + `Status.from(…)`, which is
half of #229's report.

**Precedence: a literal module path always beats an alias expansion.** The walk
must try the written path as a key in `ImportedInterfaces::modules` first, and
consult the alias table only when that misses. This is the right default on its
own, and it is what keeps `Data` unambiguous: `Units.Data` (data sizes,
`src/stdlib/units/data.kex`) ends in the same segment, so in a file importing
both `Units.Data` and `Data.Set` the alias `Data → Units.Data` would otherwise
capture a qualified `Data.Set.from(…)`. With literal-first, `Data.Set` is a real
key and resolves before the alias is ever considered. Cover this with a spec.

Do halves 1 and 2 together and verify before moving on — landing (2) without (1)
is a known regression (false "expects argument 1 to be Char" against prelude
overloads), documented in #229.

## The move

`src/stdlib/set.kex` → `src/stdlib/data/set.kex`, contents otherwise unchanged
except:

- Add the `module Data` header.
- Keep the two record backings exactly as they are — a sorted list for `Set`, a
  `{A: Bool}` map for `UnorderedSet`. kexhq/kex#209 (route these to `ordsets` /
  `sets` v2 BIFs) depends on those layouts; the header comment already explains
  why and should be kept.
- Update the header comment: `using Data.Set`, and note that the import brings
  both flavours.
- Leave `showValue` printing `Set(…)` / `UnorderedSet(…)`. It is display text,
  and the short form reads better.

## Stack

`src/stdlib/data/stack.kex`, `record Stack<A> do elements : [A] = [] end`, with
`elements` stored **top first** so `push`/`pop`/`peek` are all list-head
operations. `items` reverses, so `Data.Stack.from(xs).items == xs` and the
argument order reads bottom-to-top.

```kex
let s = Stack.from([1, 2, 3])
s.peek                # => Just(3)
s.push(4).items       # => [1, 2, 3, 4]
s.pop                 # => Just((3, Stack(1, 2)))
Stack.empty.pop       # => None
```

- `module Stack do let from(items: [A]) -> Stack<A>; let empty -> Stack<A> end`
- `make Stack<A>, implement: Enumerable, Foldable, Monoid, Showable`:
  `reduce` (top to bottom — say so in the doc comment), `identity`, `combine`
  (the argument's elements pushed on top), `showValue` as `Stack(1, 2, 3)`
  bottom-to-top.
- `push :> A -> Stack<A>`, `pop :> (A, Stack<A>)?`, `peek :> A?`,
  `count :> Integer`, `empty? :> Bool`, `items :> [A]`,
  `+ :> [A] -> Stack<A>` and `+ :> Stack<A> -> Stack<A>`.
- `make Stack<A>, implement: Blankable` with `blank? :> Bool`, mirroring
  `set.kex:413`.
- `push!` / `pop!` come free from the `!` rebinding form; document them the way
  the `Set` header comment documents `add!`/`delete!`.

`pop` answering `(A, Stack<A>)?` follows `List.first`'s `X?` convention
(`src/stdlib/list.kex:82-90`) rather than introducing a `Result`.

## Queue

`src/stdlib/data/queue.kex`, a banker's queue:
`record Queue<A> do front : [A] = []; back : [A] = [] end`, where `back` holds
the tail reversed. `enqueue` conses onto `back` and `dequeue` takes `front`'s
head, rotating `back.reverse` into `front` when `front` runs out — amortized
O(1) both ways, versus O(n) `enqueue` for a single-list queue.

```kex
let q = Queue.from([1, 2, 3])
q.enqueue(4).items    # => [1, 2, 3, 4]
q.dequeue             # => Just((1, Queue(2, 3)))
q.peek                # => Just(1)
```

- `items :> [A]` is `@front + @back.reverse`, so the queue is not opaque —
  the same promise `set.kex`'s header comment makes.
- **`==` must be overloaded** (`== :> Queue<A> -> Bool`, comparing `items`).
  Unlike `Set`, a queue's representation is *not* canonical: `Queue.from([1,2])`
  and `Queue.from([1]).enqueue(2)` hold the same elements in different
  `front`/`back` splits and are structurally unequal. Say this explicitly in the
  file header, and note the residue: two such queues used as map keys, or matched
  as record patterns, still compare structurally on both backends.
- Same trait set as `Stack`: `Enumerable`/`Foldable`/`Monoid`/`Showable` plus
  `Blankable`, with `reduce` front-to-back and `showValue` as `Queue(1, 2, 3)`.
- `enqueue`, `dequeue :> (A, Queue<A>)?`, `peek :> A?`, `count`, `empty?`, `+`.
- `enqueue!` / `dequeue!` come free from the `!` form.

## Other files to update

- `src/stdlib/prelude.kex` — the header comment lists "sets (`Set`)" among the
  opt-in libraries; make it `Data.Set`. No `using` line changes; none of this is
  in the prelude.
- `src/common/prelude_tiers.hxx:27-29` — the comment claiming "set.kex comes
  after list.kex and map.kex" is already stale (set.kex is in no tier because it
  is not a prelude source). Delete those two lines.
- `docs/language-spec.md:420-441` — the Sets section: `using Data.Set`, the new
  spellings, and a short paragraph introducing `Data.Stack` and `Data.Queue`.
- File a tracking issue with `gh issue create` before starting, per repo
  convention, and link kexhq/kex#229 and #209 from it.

## Specs

`spec/prelude/set.spec.kex` is misfiled — `Set` was never a prelude module, and
`spec/prelude/` runs with `--no-check`. Move and split:

- `spec/stdlib/data_set.spec.kex` — the existing 300 lines, retargeted at
  `using Data.Set`. Runs **with** the type checker (`spec-stdlib` only passes
  `--no-check` when the file asks for it), which is what proves the #229 work.
- `spec/stdlib/data_stack.spec.kex`, `spec/stdlib/data_queue.spec.kex` — new
  suites in the same describe/it shape.
- One spec importing two `Data` files at once, to prove three sources sharing the
  `module Data` header merge cleanly.
- One spec importing `Units.Data` and `Data.Set` together, pinning the
  literal-path-beats-alias precedence.
- Do **not** mark any of them `# kex: no-check` or `# kex: skip-beam`. If one
  needs a marker, the checker half is not finished.

## Verification

1. `cmake --build build`
2. `make test-all` — the full gate, including `spec-orphans`, `spec-stdlib` and
   `spec-stdlib-beam` (bare `ctest` skips the `.kex` suites).
3. `./build/kex -C spec/stdlib/data_set.spec.kex` — must report no diagnostics
   *and*, spot-checked with a scratch file, `let bad: Integer = Set.from([1]).count`
   must be a type ERROR. If it is accepted, receivers are still typing as
   `Unknown` and #229 is not actually fixed.
4. `./build/kex -R spec/stdlib/data_set.spec.kex` — BEAM parity; this is where a
   `map`/`filter` mis-dispatch to the prelude's `List` overload surfaces as a
   runtime failure rather than a silent Unknown.
5. `./build/kex -c -o /tmp/out spec/stdlib/data_queue.spec.kex` then run the
   emitted beam, confirming `Data.*` compiles as its own BEAM module the way
   `Net.*` does.
6. `make build-wasm && make test-wasm` if touching `prelude_interfaces.hxx` — the
   wasm build has no prebuilt artifact and takes the source-interface path
   exclusively, so it exercises the half-1 change hardest.


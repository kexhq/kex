# Cross-unit trait methods with trait params miscompile on BEAM

**Status:** open. Found while rolling `Writable` out to sockets on
`net-text-api`. `Closeable` (zero-arg methods) and concrete-param methods
(`writeBytes`) work; only methods with trait-bounded params fail.

A `make` method implemented for an opt-in type, whose name a prelude type
also implements and whose signature takes a trait-bounded parameter,
miscompiles its call sites: the call goes to the prelude home with the
trait dictionary missing, so BEAM dies `undef`.

## Repro

```kex
# In Net.Socket scope (src/stdlib/net/socket.kex):
make Net.Socket.TCP.TCPConnection, implement: Writable do
  printLine :> Showable -> Void
  foul printLine(content) do
    let payload = (content.showValue + "\n").to(Binary).try
    Kex.Intrinsic.NetTCP.sendAll(this, payload).try
    ()
  end
  # ... print, writeLine, write, writeBytes
end
```

```kex
# Any caller:
using Net.Socket

main do
  let listener = TCP.bind("127.0.0.1:0").try
  let client = TCP.connectTo("127.0.0.1:${listener.localAddress.try.port.string}").try
  client.printLine("hello")   # Internal error: undef
  client.close
  listener.close
end
```

`--emit-core` shows the call lowered as
`call 'kex_prelude':'printLine'(Client, Binary)` — wrong unit (the
prelude's `FileHandle` method) and wrong arity (`/2`; the prelude only
exports `/3`, whose third argument is the dictionary). The identical call
on a `FileHandle` receiver correctly emits `kex_prelude:printLine/3`.

## What works (the control group)

- `close` (zero-arg) on all six socket handle types: green
  (`spec/closeable_traits_beam.kex`).
- `writeBytes` (concrete `Binary` param) as a method: green once the
  dispatcher tags were fixed (see below).
- `Headers.add` (plain params, unique name): green.
- Free functions of any shape, including the same bodies: green.

So the trigger is specifically: colliding method name + trait-bounded
parameter + opt-in home unit.

## Analysis

Two defects, found in that order:

1. **Dispatcher tags need fully-qualified owners (fixed in stdlib).**
   `make TCP.TCPConnection` at `Net.Socket` scope generated dispatcher
   guards on `'TCP.TCPConnection'`, but values carry
   `'Net.Socket.TCP.TCPConnection'`, so every call fell through to
   `kex_prelude` and died `Undefined method: m for Tuple`
   (`src/ir/lower.cxx` `typeGuard`: the scope-qualification at
   `:10095` only fires for bare names). Writing the targets fully
   qualified (`make Net.Socket.TCP.TCPConnection`) fixes the guards.
   All socket `make` blocks now use fully-qualified targets.

2. **Caller resolution drops the dictionary (open, this issue).** With
   correct tags, `writeBytes` works but `printLine` still emits
   `kex_prelude:printLine/2`. The checker resolves the call through
   the `winnerIsMakeMethod` path (`src/semantic/typechecker.cxx`
   `:10320`), which sets `backendArity = params.size()` and never
   attaches `traitDictionaries` (compare the export path at `:10267`,
   which does). Lowering then emits the prelude home at dictionary-less
   arity. The `stalePreludeTarget` correction
   (`src/ir/lower.cxx:4270`) never fires because it requires a
   `kex_prelude` backend module on the resolution, while this path
   leaves it empty.

Related, possibly the same family: same-name UFCS calls from inside a
sibling module resolve to the first module's overload
(`connection.receiveLine(n)` with a `UnixConnection` receiver, called
from inside `module Unix`, binds `TCP.receiveLine` and fails checking;
`connection.sendAll` with no overloads resolves fine). Stdlib code
calls potentially-colliding names qualified or via intrinsics.

## Workaround in use

Socket text writes stay free functions (`TCP.write`, `Unix.printLine`,
…), called through UFCS exactly as before. `Closeable` methods ship
(zero-arg methods resolve correctly). Revisit `implement: Writable`
for `TCPConnection`, `UnixConnection`, and `TLSConnection` once this
is fixed; `spec/writable_sockets_beam.kex` exercises the same five
methods per transport and converts mechanically.

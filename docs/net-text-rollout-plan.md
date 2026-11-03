## Goal

Roll the `net-text-api` branch's TCP text simplification across the socket API as shared constructs: one `host:port` parser, `Closeable`/`Writable` traits implemented by every handle type, string addresses and autoclose blocks on all stream constructors, and `capability` conversion so net becomes mockable. All on the current branch, stdlib (`.kex`) only — no backend changes.

## Success Criteria

- `TCP.connectTo`/`bind`, `UDP.bindTo`, `Unix.connectTo`/`bind` all accept strings; `UDP.Endpoint.parse` exists; all share one split implementation.
- `FileHandle`, `TCPConnection`, `TCPListener`, `UnixConnection`, `UnixListener`, `TLSConnection`, UDP `Socket` implement `Closeable`. `TCPConnection`, `UnixConnection`, and `TLSConnection` expose the `Writable` five (`printLine`/`print`/`writeLine`/`write`/`writeBytes`) as free functions with identical UFCS call sites; true `implement: Writable` is deferred on `docs/issue-cross-unit-trait-dict.md` (BEAM miscompiles cross-unit trait-dict method calls).
- String constructors have block forms that autoclose, following `FS.File.open` exactly (param named `block`, signature line plus bare def, Error-first arms, close on return, explicitly not on raise).
- `TCP`/`UDP`/`Unix`/`TLS` are capabilities with `Mock.*` stand-in records; `with Net.Socket.TCP = ...` intercepts module calls, and the text-layer routing question below is resolved with a passing spec.
- Existing suite green (`make test`, `make spec`), plus new specs for each piece; no qualified/bare call-site breakage (verified: all current call sites use UFCS).

## Approach

Mirror proven shapes instead of inventing: `FS.File.open`'s overloads + block form (including its documented non-ensure), `Mock.Files`' fixture-plus-hook records, and the branch's own `getLine`/`printLine` bodies. Trait methods call intrinsics directly with `this` (the `websocket.kex`/`dns.kex` precedent) rather than `this.`-UFCS, which has zero precedent in the stdlib. Reuse the reserved-but-unconsumed `Mock.Net.Socket.Script` record as the canned-data shape.

## Steps

1. **Shared plumbing.** Add `HostPort` + `splitHostPort(address, operation)` directly under `module Net.Socket`; refactor `TCP.Endpoint.parse` onto it. Guarded by the existing `spec/net_socket_text_beam.kex`.
2. **String addresses.** `UDP.Endpoint.parse` (via the helper) + `UDP.bindTo`; `Unix.connectTo`/`Unix.bind` (via `Address.path`). Names forced by #619 (`bind` taken in UDP at same arity). New beam specs mirroring the TCP one.
3. **Unix `getLine`.** Free function mirroring TCP's (via existing `receiveLine` + decode/trim). Spec with a loopback pair.
4. **`Closeable`.** Add close-only trait in `filehandle.kex`; move `FileHandle.close` into an implementing block; add implementing blocks for all six socket handle types and remove the six free `close` functions. First compiler check of the make-block move.
5. **`Writable` rollout (rescoped).** The `Writable` five as free functions for `TCPConnection`, `UnixConnection`, and `TLSConnection` (TCP keeps its branch bodies plus the `print`/`writeLine` aliases; Unix/TLS mirror them). UDP stays out (peerless trait, peerless-less socket). True `implement: Writable` blocks are deferred on `docs/issue-cross-unit-trait-dict.md` — implemented, verified broken on BEAM (`kex_prelude:printLine/2` miscompile), reverted to free functions per user decision. Socket `make` targets must be fully qualified (`make Net.Socket.TCP.TCPConnection`) or BEAM dispatcher tags mismatch at runtime.
6. **Autoclose blocks.** `(String, Block)` overloads on the five string constructors from steps 2 + branch, written exactly in the `FS.File.open` block shape (param named `block`, signature line plus bare def, Error-first match arms). Endpoint/options forms deliberately excluded (arity-2 collision per #619). Specs for value-passthrough (`Ok` carries body's value) and open-failure-skips-block.
7. **Capability + mocks.** `module` → `capability` for the four transports (`Port`/`Support` stay modules — pure). Add `Mock.TCP`/`UDP`/`Unix`/`TLS` records (fixture + `onSend`-style hooks) with full member implementations; resolve the routing flag: verify whether trait methods are intercepted under `with` (IO.out precedent) and spec whatever the true behavior is.
8. **Full validation.** `make spec`, `make test`, plus the branch's beam spec.

Dependencies: 1 → 2 → 6; 4 → 5 (Closeable proves the move pattern first); 5 → 7 (routing flag needs real methods to test against). 3 is independent after 1.

## Validation Plan

- Per step: rebuild (`cmake --build build`) and run the touched specs (beam specs via the `make spec` runner, which dispatches on the `# kex: run-beam` marker per `Makefile:187`).
- Steps 4–5: the compiler itself is the check — trait satisfaction, no erasure collision from removed/added methods; then `make spec` for regressions.
- Step 7: a new spec that substitutes a mock transport under `with` and asserts interception (or documents the true routing if methods bypass mocks).
- Highest-risk validation: step 7's routing spec — it decides whether trait methods need rework to stay mockable.

## Risks / Open Questions

- **Trait-method mock routing.** Unknown until step 7 whether `with TCP = ...` intercepts make-block methods or only module calls. If it doesn't, methods must delegate to free functions (kept as the capability surface) — small rework, contained in step 7.
- **UFCS quirk (found in step 3).** `connection.receiveLine(65536)` with a `UnixConnection` receiver, called from inside `module Unix`, resolved to `TCP.receiveLine` and failed typechecking. Same-name cross-module UFCS calls from inside a sibling module do not dispatch on the receiver — stdlib code must call the intrinsic (or a same-module function) directly there. User call sites in specs are unaffected (verified: `peer.receiveChunk`/`sendAll` on Unix handles resolve correctly from spec scope).
- **Non-goals (decided):** net typestate (rejected — constructors offer no choice); `closed?` in `Closeable` (socket `closed?` means owner-liveness, no file equivalent); error-generic read traits (needs `implement: Trait<Args>` language work); connected UDP; TLS `getLine` (blocked on a `receiveLine` intrinsic — backend work); scripted-connection backend devices (files have mock devices, net has none — the follow-up that makes mocks genuinely useful, since a refusing stand-in voids socket mocks entirely).
- **`implement: Writable` for sockets (deferred, not dropped).** See `docs/issue-cross-unit-trait-dict.md`. Revisit after the checker fix; `spec/writable_sockets_beam.kex` converts mechanically (restore the `report(out: Writable, …)` bound for sockets).

## As-Built Deviations

- **"No backend changes" did not survive step 7.** Four compiler fixes were required; without them `with` substitution over net mocks is unimplementable in any `.kex` shape:
  1. `src/common/prelude_interfaces.hxx`: the source-path trait-claim scan only qualified module-nested ADT targets, so `make TCP, implement: Net.Socket.TCP` inside `module Mock` registered a bare `TCP` conformance that never matched `Net.Socket.Mock.TCP`. Claims now honor absolute multi-part spellings (plus the bare back-spelling, because imported signatures type values exactly as written) and fall back to the owner-qualified spelling (KexI-path parity, kexhq/kex#143 shape).
  2. `src/ir/lower.cxx`: block-form capability calls (`connectTo(addr) { … }`) skipped substitution dispatch on all three BEAM routes (`!n.block` guard from the original #180 lowering) and silently reached the real implementation. The trailing block is now a dispatched argument like any other. Named-argument capability calls still go direct (unchanged, out of scope).
  3. `src/interpreter/evaluator.hxx`: the walker's `canonicalCapability` ignored `using`, so bare `TCP.connectTo` after `using Net.Socket` missed the `with` binding. It now tries `using`-module prefixes (BEAM-parity candidate expansion).
  4. `src/semantic/typechecker.cxx` (`knownSomewhere`): imported capabilities synthesize their interfaces as traits, whose member names then suppressed "undefined method" for any receiver in any module — converting the socket modules exposed it (`running.localAddress` checked clean despite `except: [Server]`). Only a capability in reach may now vouch for a name; ordinary traits keep historical behavior.
- **Routing question (step 7 risk) resolved:** `with` intercepts plain and block-form capability members on both backends, for bare and qualified call spellings. Trait (`Closeable`) methods bypass `with` by design — proven by `spec/mock_bypass_beam.kex`.
- **Mocks live in `src/stdlib/net/socket.kex` (`Net.Socket.Mock`), not top-level `mock.kex`:** the artifact build covers top-level files only, and a top-level mock cannot see subdir `Net.Socket` declarations. Failure-injection stand-ins (`failure: NetError?`, canned `MockEmpty`); scripted success still needs backend devices (non-goal, unchanged).
- **Operation args inside `module Mock` must be written `Net.TCP`-qualified:** bare `TCP` there binds the `Mock.TCP` record, not the `NetOperation` variant.

# `serving` — Typed Servers Plan

> 📋 **Design doc — not yet implemented.** Nothing here is built. Two *live
> bugs* are recorded under "Current state" and are worth fixing independently of
> whether `serving` ever ships.

## Context

Erlang and Elixir cannot type process messages. `GenServer.call(pid, {:increment, 12})`
is unchecked at every step — wrong pid, wrong message tag, wrong arity, wrong
reply handling are all runtime crashes. That is the largest single gap a
statically typed BEAM language can close, and it is squarely on the stated goal
of being "much better at static typing".

`serving` attaches typed message handlers (**slots**) to a record, exactly as
`make` attaches methods:

```kex
record Counter do
  count : Integer = 0
end

serving Counter do
  slot increment(by: Int) -> Reply<Int> do
    new.count = @count + by
    return { new, reply: new.count }   # next state AND reply, in one value
  end

  slot get -> Reply<Int> = { reply: @count }   # state unchanged
end

let counter = Process.spawn(Counter { })   # Server<Counter>
counter.increment(12)                      # checked: slot exists, args match, reply is Integer
```

### The goal, stated as an acceptance criterion

**A gen_server, but much shorter, with typing that is easy and useful.** Same
Counter in both:

```elixir
defmodule Counter do                                    # Elixir — untyped
  use GenServer

  def start_link(n \\ 0), do: GenServer.start_link(__MODULE__, n)
  def increment(pid, by), do: GenServer.call(pid, {:increment, by})
  def get(pid),           do: GenServer.call(pid, :get)
  def reset(pid),         do: GenServer.cast(pid, :reset)

  @impl true
  def init(n), do: {:ok, n}

  @impl true
  def handle_call({:increment, by}, _from, n), do: {:reply, n + by, n + by}
  def handle_call(:get, _from, n),             do: {:reply, n, n}

  @impl true
  def handle_cast(:reset, _n), do: {:noreply, 0}
end
```

```kex
record Counter do                                       # Kex — fully typed
  count : Integer = 0
end

serving Counter do
  slot increment(by: Int) -> Reply<Int> do
    new.count = @count + by
    return { new, reply: new.count }
  end
  slot get   -> Reply<Int> = { reply: @count }
  slot reset do new.count = 0 end
end
```

Roughly half the lines, but the structural difference matters more than the
count: **gen_server declares every operation twice** — a client function and a
handler clause — and nothing checks that they agree. Rename the tag in one and
the other fails at runtime, in production, on that one code path. `serving`
declares each operation once and generates the client side, so the two cannot
drift.

The typing is the other half. In the Elixir version, `Counter.increment(pid, "x")`,
`Counter.incrment(pid, 1)`, and treating the reply as a `String` are all runtime
failures. In the Kex version all three are compile errors, and none of it
required writing a type by hand beyond the slot signature you would write as
documentation anyway — which is what "easy and useful" has to mean: types that
come from declarations you already wanted to write.

**Acceptance criterion:** if the design grows to where a Kex server is as long
as its gen_server equivalent, or where making it typed requires annotations
beyond the slot signatures, it has failed its purpose.

## Decisions (locked)

- **`serving X do ... end` parallels `make X do ... end`.** Same shape, different
  aspect: `make` attaches methods, `serving` attaches slots. The record already
  declares the state, so there is no separate `state` block.
- **`slot` is legal only inside a `serving` block.** A `slot` in a plain `make`
  has nothing to dispatch on, so it is a compile error rather than a construct
  with defined behaviour.
- **`serving` stays its own block form.** Folding it into `make` was considered
  — `make SessionStore, serving: true`, or a magic `implement: Server` trait —
  on the grounds that the call site already distinguishes remote from local via
  the receiver's type (`Server<SessionStore>` vs `SessionStore`), so the keyword
  adds nothing a reader lacks. Rejected: `slot` needs a block that licenses it,
  and a dedicated `serving` is the clearest thing to attach that rule to.
- **Slots have local and remote call projections.** Calling a slot on its state
  record executes the transition directly; calling it on `Server<State>` sends
  a message. Local sibling calls are therefore safe. Messaging a handle that
  turns out to be the current server can still deadlock and cannot in general be
  detected statically.
- **A name may not be both a `slot` and a `let` in the same block** — it is a
  compile error. A bare `name :> T` annotation could not say which it belongs
  to, and neither could a caller inside the block.
- **Slots are named protocol transitions, not first-class functions.** They have
  callable local and remote projections, but several restrictions follow:

  - **No first-class references.** `&.increment`, `~increment`, or passing a slot
    as a value do not apply — there is no function to reference.
  - **Two receiver-directed projections.** `Counter.increment` is a local state
    transition; `Server<Counter>.increment` is a remote message call. A `make`
    method of the same name remains unambiguous because ordinary record method
    dispatch prefers the `make` method; the slot transition remains available
    inside its `serving` block and through generated serving metadata.
  - **`::>` is not a function type.** `fetch ::> String -> Reply<Session?>`
    describes a protocol entry, not a `String -> Reply<Session?>` value.

- **`::>` optionally annotates slots that have a `from`.** An inline `slot`
  signature is already complete and is the normal single-clause form. A
  separate annotation is useful for multi-clause slots; writing both is
  allowed only when their types agree. When present, the rule is mechanical —
  **colons count implicit parameters**: `:` binds none, `:>` binds
  `this : This`, `::>` binds `this : This` and `from : From<T>`. Casts annotate
  with `:>`, since they have no `from`; `::>` on a cast is an error.

  Slots are accepted as **a special case in the language** — that is the
  justification for a third sigil. They are the one construct with two implicit
  parameters, a different receiver at the call site (`Server<This>` rather than
  `This`), and a return type that is unwrapped for callers. A sigil that says
  which of those apply is worth more than the cost of two similar-looking
  markers, and it is what makes the call-site type mechanically derivable for
  type-based search.
- **A call returns `Result<T, CallError>`; `.try` opts into crashing.** The
  typed form is unmarked so it is what gets written by default; let-it-crash is
  four characters away and reuses the existing `.try` / `trying` / `Tryable`
  machinery rather than adding a slot-specific one.
- **`slot` and `let` coexist inside a `serving` block.** `slot` declares a
  message handler (part of the wire protocol); `let` declares an internal helper
  that is not reachable by message. Without the distinction every declaration
  would join the protocol, and opting out would need visibility blocks or a
  naming convention instead. Inferring it from the return type does not work:
  casts have no return type and would be indistinguishable from helpers.
- **Slots reuse the `new` operator unchanged.** `@field` reads current state,
  `new.field = ...` sets the next state, exactly as in a `make` block
  (`docs/new-operator-plan.md`). The server's state *is* the receiver.
- **Spawning is explicit.** `Process.spawn(Counter { })`, not construction with
  a hidden effect. `Counter { }` stays a pure record; the effect lives in
  `Process.spawn`, which is `foul` like the existing `Task` module. The initial
  state is visible at the spawn site.
- **The handle is typed.** `Process.spawn : X -> Server<X>`. This is the piece
  that makes everything else checkable — see Design.
- **Generated servers conform to `gen_server`.** Slot calls use OTP's
  `$gen_call` / `$gen_cast` protocol, and generated servers implement `init/1`,
  `handle_call/3`, `handle_cast/2` and `start_link`. No private envelope, no
  private tag.

  Sender-inside-the-message rather than in a wrapper is the right instinct, and
  `$gen_call` already specifies exactly that, so conforming costs nothing at the
  Kex call site — both ends are generated either way. What it buys: Erlang and Elixir
  can call Kex servers, OTP supervisors can supervise them, `:observer` and
  `sys` can inspect them, and `{'kex_msg', ...}` disappears entirely.
- **Interop is a requirement, not a nice-to-have.** Kex must be able to send to
  and receive from plain Erlang/Elixir processes, including OTP system messages.

## Current state (grounded)

Verified against the tree.

- **`Pid` is opaque and untyped** — `process.kex:2`, "Pid is an opaque BEAM
  process identifier". `Process` exposes `self`, `exit`, `register`, `whereis`,
  plus `make Pid` with `link`/`unlink`/`monitor`/`demonitor`.
- **There is no `Process.spawn`.** The existing primitive is
  `Task.start : Block<X> -> Task` (`process.kex:47`), inside `module Task`.
- **Every message is wrapped.** `send` emits `{'kex_msg', Msg, erlang:self()}`
  (`runtime/src/kex_intrinsic_process.erl:10`), built at `lower.cxx:1406`, and
  `receive` matches only that shape (`emit_core.cxx:281`, documented at
  `ir.hxx:170`). Four sites in total.

### 🐞 Live bug 1 — OTP system messages are unreceivable

`Process.monitor` calls `erlang:monitor(process, Pid)` and `link` calls
`erlang:link/1`. The VM then sends `{'DOWN', Ref, process, Pid, Reason}` and
`{'EXIT', Pid, Reason}` — **unwrapped**, because the VM knows nothing about
`kex_msg`. Kex's `receive` matches only `{'kex_msg', Payload, Sender}`, so those
messages can never match any clause.

`monitor` and `link` are therefore exposed but their notifications are
unreachable. This undercuts the fault-tolerance story directly: supervision is
built on exactly these messages.

### 🐞 Live bug 2 — mailbox leak

Selective receive leaves unmatched messages in the mailbox. Because a `DOWN`
can never match, a long-lived process that monitors children **accumulates them
forever** — unbounded memory growth, not merely a missing feature. Same for
`EXIT` under trap_exit, and for any message sent by a non-Kex process.

Both bugs have the same root cause and the same fix.

## Design

### Typed handles are the load-bearing piece


`Server<X>` carries the slot table of `X`, so the checker can verify at the call
site that the slot exists, that argument types match, and what the reply type
is. All four of Elixir's runtime failure modes become compile errors. Without
the type parameter none of the rest is checkable, and `serving` degenerates into
`GenServer` with nicer syntax.

### Slots: reply versus state (decided)


A slot returns **both**, in one value: the next state and the reply.

```kex
slot increment(by: Int) -> Reply<Int> do
  new.count = @count + by
  return { new, reply: new.count }
end

slot get -> Reply<Int> = { reply: @count }   # state unchanged — omitted

slot reset do                                 # no Reply<_> ⇒ cast
  new.count = 0
end
```

The shape carries the meaning, with nothing implicit and no new keyword:

| Written | Means |
|---|---|
| `-> Reply<T>` | a call; replies with `T` |
| no return type | a cast; asynchronous, no reply |
| `new` in the returned value | state changed |
| `new` omitted | state unchanged |
| `stop:` in the returned value | terminate with that reason |
| `stop:` omitted | keep serving |

For a local call the compiler materializes a `Transition<State, Reply>` value.
Its public fields are `state`, `reply`, and optional `stop`; the contextual
`new` shorthand in a slot result becomes `state` on that value. For a remote
call the runtime installs `state`, acts on `stop`, and sends only `reply` back.
Casts use `Transition<State, Void>`. A call slot is locally callable only when
every successful exit replies immediately. A deferred-capable slot needs a real
OTP `from` reference and has only the remote projection.

### Terminating a slot


A hand-written Erlang loop quits by simply *not recursing*. `serving` generates
the loop, so termination cannot be control flow — it has to travel in the
returned value, as `gen_server`'s `{:stop, Reason, State}` does.

The returned record already has optional fields, so `stop:` joins them:

```kex
slot shutdown -> Reply<Void> do
  return { stop: :normal, reply: unit }    # reply, THEN terminate
end

slot fatal(reason: Atom) do
  return { stop: reason }                  # cast — terminate, no reply
end

slot drain -> Reply<Integer> do
  return { new, reply: @pending.count } if @pending.any?
  return { stop: :normal, reply: 0 }       # stop once drained
end
```

`stop` with `reply` is `{:stop, Reason, Reply, State}`; `stop` alone is
`{:stop, Reason, State}`. No new keyword, and the "shape carries the meaning"
rule holds.

Open: whether a `terminate`/cleanup hook is needed (gen_server has one, for
releasing resources on the way out), and whether an uncaught error in a slot
body should crash the process — letting a supervisor restart it, which is the
OTP-idiomatic answer — or be convertible to a `stop`.

**Why this over the alternatives.** Four other shapes were considered:

1. `->` types the reply and `new` is harvested implicitly. Terser, but it makes
   `new` mean two different things — in a `make` block `new` matters *because*
   you return it, while here it would take effect without being returned. That
   is a bad inconsistency in a keyword this central. **`Reply<T>` avoids it
   precisely because `new` IS returned**, inside the returned value.
2. Return a bare tuple `-> (Int, This)`. Honest but positional, and every
   read-only slot pays `return (@count, this)`.
3. `->` types the reply, state set by a `become(...)` call. Explicit, but adds a
   keyword and invites "may I call it twice / not at all".
4. Every slot returns `This`, replies via `reply(...)`. Uniform, but needs a
   second mechanism for replies and leaves `slot get` returning unchanged state
   as pure ceremony. **Rejected.**
5. Separate `query` / `slot` / `cast` declaration forms, letting the checker
   enforce that a query cannot mutate. **Rejected** — three declaration forms
   for one concept, and the guarantee does not pay for the extra surface.

### Slots are multi-clause, like any Kex function


Slot parameters pattern-match, and a slot may have several clauses. This is the
closest correspondence to Erlang, where `handle_call` clauses *are* the pattern
match — except here the message type is declared and checked.

The shared signature goes on a `:>` annotation line, exactly as in a `make`
block (`filehandle.kex` uses this form), and the clauses follow:

```kex
type Command = Deposit(Integer) | Withdraw(Integer) | Balance

record Account do
  balance : Integer = 0
end

serving Account do
  apply ::> Command -> Reply<Result<Integer, AccountError>>

  slot apply(Deposit(n)) do
    new.balance = @balance + n
    return { new, reply: Ok(new.balance) }
  end

  slot apply(Withdraw(n)) do
    return { reply: Error(Insufficient) } if n > @balance
    new.balance = @balance - n
    return { new, reply: Ok(new.balance) }
  end

  slot apply(Balance) = { reply: Ok(@balance) }
end
```

All clauses of one slot share a single signature, so the reply type is uniform
across them — `Result<Integer, AccountError>` here, which is why the
insufficient-funds case is a value rather than a different return type.

Literal and wildcard patterns work as anywhere else:

```kex
slot retry(0) -> Reply<Bool> = { reply: false }
slot retry(n) -> Reply<Bool> do ... end
```

**Guards are not available in clause heads.** `slot f(x) when x > 0 do` does not
parse — Kex allows `when` in `match` arms only, not in `let`/`slot` heads
(verified: `let step(0, n) when n > 0 = ...` is a syntax error). Conditions go
inside the body, via a guarded `return ... if cond` or a `match`.

**Open: non-exhaustive clauses.** Erlang crashes with `function_clause` and lets
the supervisor restart, which is idiomatic on BEAM. The alternative is requiring
exhaustiveness at compile time, which is stronger but only possible when the
message type is closed (an ADT like `Command` above, not `Integer`). Worth
deciding alongside the general exhaustiveness story — `todos.txt` already notes
that literal and tuple exhaustiveness are missed today.

### Annotating slots — `::>` (decided)

These annotations are optional. `slot fetch(id: String) -> Reply<Session?>`
defines the complete protocol on its own. The separate form is primarily for
grouping one signature above several clauses; it must not force users to repeat
that signature on each implementation.


`:>` means "implicit `This` as first param" (grammar.ebnf:177). For a `let` that
is the whole story. For a slot the same sigil would hide considerably more:

```kex
lookup :> String -> Result<Session, NotFound>   # let:  This -> String -> Result<...>
fetch  :> String -> Reply<Session?>             # slot: (This, From<Session?>) -> String -> Reply<Session?>
                                                #       and CALLERS see Server<This> -> String -> Session?
```

Three differences hidden behind one sigil: a second implicit **parameter**
(`from`), a different receiver at the call site (`Server<This>`), and the reply
unwrapped from `Reply<...>`. (`new` is not among them — it is a local derived
from `this`, not a parameter, so it is absent from both signatures.)

There is also a case that is genuinely ambiguous rather than merely subtle —
**casts**, which carry no `Reply<T>` to mark them:

```kex
enqueue :> Job -> Void    # a cast slot? or an internal helper returning Void?
```

`::>` resolves all of it, and the three forms become self-describing:

| Annotation | Meaning |
|---|---|
| `lookup :> String -> Result<...>` | internal helper |
| `fetch ::> String -> Reply<Session?>` | call slot — caller sees `Session?` |
| `enqueue ::> Job -> Void` | cast slot — no reply |

The mnemonic holds: the extra colon is the extra hop, through a process.

**The rule: colons count implicit parameters.**

| Sigil | Implicit parameters |
|---|---|
| `:` | none |
| `:>` | `this : This` |
| `::>` | `this : This`, `from : From<T>` |

Mechanical rather than categorical — the sigil states exactly what it binds, not
what kind of thing is being declared. Consequences:

- **Casts annotate with `:>`**, since they have no `from`.
  `::>` on a cast is an error: there is nothing to bind.
- `from`'s type parameter is **determined by the signature**, never written:
  `fetch ::> String -> Reply<Session?>` gives `from : From<Session?>`. That is
  what makes `from.reply(x)` checkable.

```kex
lookup  :> String -> Result<Session, NotFound>    # helper    — this
enqueue :> Job -> Void                            # cast slot — this
fetch  ::> String -> Reply<Session?>              # call slot — this, from
```

**Accepted cost:** a standalone cast annotation is indistinguishable from a
helper annotation. This only bites for annotations with no definition following
— `slot enqueue` versus `let enqueue` settles it otherwise — and those occur
essentially only in traits, where trait-required slots remain speculative.

Considered and rejected: `::>` meaning "reachable by message", binding `from`
only when the return type was `Reply<T>`. That makes the sigil's meaning depend
on the return type; counting implicits is uniform.

**Typos behave predictably**, which answers the main objection to having two
similar sigils: `:>` where `::>` belongs leaves `from` unbound, so using it
errors; `::>` on a cast errors. Neither silently changes meaning.

**Searchability is a hard requirement** — users must be able to search by type
definition — and it constrains this design, so record what it implies.

The **searchable type of a slot is its call-site type**, not its handler type.
Someone looking for "how do I get a `Session` from a `String`" is thinking about
the call:

```kex
fetch ::> String -> Reply<Session?>
```

| View | Type |
|---|---|
| handler — what the body sees | `(SessionStore, From<Session?>) -> String -> Reply<Session?>` |
| **call site — what search indexes** | `Server<SessionStore> -> String -> Session?` |

The indexer derives the second mechanically: substitute `This`, drop `from`,
unwrap `Reply<...>`. That derivation is only unconditional because the
colons-count rule makes the implicits recoverable from the sigil alone — a second
reason to prefer it over a sigil whose meaning depended on the return type.

Two consequences:

- **`let` helpers must be excluded from the index** — they are internal, not API.
  Since cast slots and helpers share `:>`, the annotation alone cannot separate
  them; the indexer needs the `slot` vs `let` keyword. Signatures in isolation
  are therefore *not* a sufficient index source. `tools/docgen/PLAN.md` already
  parses sources via `Parser.parseFile`, so this is consistent with the existing
  approach.
- **`Reply<T>` should be unwrapped in docs and search results**, since users
  think in terms of what they receive, not the handler's return envelope.

**"Slots are not formal functions" does not exclude them from type search.** That
premise is about semantics — no first-class references, absent from the UFCS
overload set, not directly invocable. The signature remains a well-formed type
expression and must be queryable.

Index **both projections**, since either is a reasonable thing to type:

| Query | Matches |
|---|---|
| `Something -> Reply<Int>` | the signature as written — what docs display |
| `Server<Counter> -> Something -> Int` | the derived call-site type — what a caller writes |
| `Something -> Int` | either, with receiver and envelope elided |

Someone reading docs sees the first, someone reasoning about a call site thinks
in the second, and someone asking "what turns a `Something` into an `Int`"
should find the slot from the third.

**Residual cost:** `:>` and `::>` differ by one character and are both valid in
the same position. Unlike the `new`/`New` pair — different syntactic categories,
where the confusable form is a defined error — these are distinguished only by
what they bind. Mitigated by the fact that neither typo is silent: each produces
an error at the point of use (see above).

**Alternative considered:** keep one sigil and mark casts in the type instead
(`enqueue :> Job -> Cast`), so `Reply<T>` and `Cast` both signal slot-ness.
Cheaper, but it leaves the receiver difference invisible.

### Blocking, failure and timeouts


Blocking follows from the shape, not from a separate decision:

| Slot | Blocks? |
|---|---|
| `-> Reply<T>` | **yes** — the caller waits for `T` |
| no return type (cast) | no — fire-and-forget |

**A call returns `Result<T, CallError>`; `.try` opts into crashing** (decided).

```kex
counter.increment(12)          # Result<Int, CallError> — failure is in the type
counter.increment(12).try      # Int — throws on Error; uncaught ⇒ the process crashes
```

Both goals are served with no new machinery. The default signature tells the
truth about timeouts and dead servers, and four characters buys `gen_server:call`
behaviour — an uncaught `TryException` crashing the process *is* let-it-crash.
Inside a function returning `Result`, or a `trying` block, `.try` propagates
instead, which is frequently what is actually wanted.

**Why this direction and not the reverse.** Crash-by-default with an opt-in
`Result` cannot be recovered from: once the default discards the error there is
nothing to inspect without a second API, so every slot would need two client
functions. Result-by-default loses nothing — `.try` is always available.

**Rejected: `call` / `tryCall` (or generated `increment` / `tryIncrement`).** It
inverts which form is *marked*:

| Scheme | Unmarked — what gets written by default | Marked |
|---|---|---|
| `.try` postfix | `increment(12)` → `Result` — **typed** | `.try` → crash |
| `call` / `tryCall` | `increment(12)` → crash — **untyped** | `tryCall` → `Result` |

Under `tryCall` the safe form costs extra keystrokes, so the path of least
resistance discards the error — the opposite of the project's stated goal. It is
also a second convention for what `.try` / `trying` / `rescue` / `Tryable`
already express, existing only for slots, and it doubles the generated surface
in docs, completion and type search.

Its one real advantage — the safe variant is discoverable by name rather than
requiring knowledge of a postfix — is weakened by `.try` being a general
language feature people already meet through `Integer.parse` and friends.

**Cost accepted:** call sites that do not care about failure write `.try`.
Elixir users write `GenServer.call(pid, msg)` with no ceremony; here the
equivalent is four characters longer. On a module making many server calls that
is visible noise.

**Composes with the rest without special cases:** a timeout is simply
`Error(Timeout)` in the `Result`, so `.try` turns it into a crash and a `match`
handles it — no separate mechanism for timeouts, dead processes or node loss.

**Timeouts — the open part.** A default (gen_server uses 5000ms and exits on
expiry) plus a per-call override is right, but the obvious spelling collides
with slot arguments, since slots take named arguments too:

```kex
counter.increment(12, timeout: 10_000)   # is `timeout` a slot param or a call option?
```

Three ways out:

- **Handle-level default** — `Process.spawn(c, timeout: 10_000)`. Coarse, but
  no namespace collision.
- **A distinct per-call form** — e.g. `counter.increment(12).within(10_000)`.
  Collision-free and local.
- **Reserved option names** — rejected: it silently breaks any slot that wants a
  parameter called `timeout`.

Decided: support both a handle-level default and a per-call override:

```kex
let slow = counter.within(10_000)
slow.increment(12)                 # 10 seconds
slow.get(within: 500)              # one-call override
```

Precedence is per-call `within:`, then the immutable handle's timeout, then the
5000ms default. `within: :infinity` disables the timeout. The name `within` is
reserved from slot parameters; an explicit override on a cast is an error.

### Purity


A slot body is a **pure function** `(state, args) -> (state, reply)`. It reads
`@field`, returns the next state and a reply, and does no IO inherently — the
same split as Elm/Redux, where the update is pure and the runtime is not.

| | Purity |
|---|---|
| slot **body** | **pure by default**; `foul slot` when it genuinely does IO |
| slot **call** (`counter.increment(12)`) | **always foul** — it is a message send |
| `Process.spawn` | foul |

Body purity and call purity are independent, which is the one surprise: a pure
`make` method is callable from pure code, but a pure *slot* is not, because
reaching it requires messaging.

**The payoff is testability.** Immediate pure slot bodies can be tested with no
process at all — the transition is directly callable on state:

```kex
let result = Counter { count: 5 }.increment(3)
assert(result.state == Counter { count: 8 })
assert(result.reply == 8)
```

gen_server allows testing `handle_call/3` directly but does not encourage it,
and nothing prevents a handler doing IO. Here it is the default and the checker
enforces it. Deferred-reply slots require a real caller reference and therefore
have only the remote projection; a local call to one is a compile error.

Plenty of real slots will be `foul` — a session store hitting Redis, a queue
writing to Postgres. The enforcement machinery already exists (`Cannot call foul
function 'IO.printLine' from pure context` is a live error today), so this is a
default to apply, not a mechanism to build.

### Calling other slots, and doing IO


**A slot must not message its own server.** `GenServer.call(self(), ...)`
deadlocks — the process is busy handling the current call and cannot service its
own message. Local slot calls and `let` helpers execute directly and are safe:

```kex
serving SessionStore do
  slot touch(id: String) -> Reply<Result<Session, NotFound>> do
    return { new, reply: @lookup(id) }      # plain call, no message
  end
  slot fetch(id: String) -> Reply<Session?> = { reply: @lookup(id).ok }

  let lookup(id: String) -> Result<Session, NotFound> = ...
end
```

This is the second reason for the `slot`/`let` split.

**Decision: a slot may invoke another slot locally.** Dispatch through the state
receiver is an ordinary transition, not a message. Only dispatch through a
`Server<X>` handle messages a process.

What is and is not detectable:

| Written inside a slot | Result |
|---|---|
| `someSlot(...)` — a sibling slot, unqualified | local transition; allowed |
| `this.someSlot(...)` | local transition; allowed |
| `@lookup(...)` — a `let` helper in the same block | fine, a plain call |
| `@bumped(3)` — a `make` method of the state type | **fine**, a plain call on the state value |
| `otherPool.checkout()` where `otherPool : Server<ConnPool>` | allowed — a different process |
| a handle that happens to be self at runtime | **undetectable** — deadlocks |

The rule reads consistently: **a direct function call is fine; a message to
yourself is not.**

### `from` — the deferred reply handle

Maps straight onto gen_server: `From` there is `{Pid, Ref}`, and
`GenServer.reply(From, Result)` sends `{Ref, Result}` to the caller. Nothing to
invent, only to type.

**`from` is implicit, exactly like `this`.** Two kinds of implicit name are in
play, and only one kind is part of the type:

| Name | Kind | In the signature? |
|---|---|---|
| `this` | implicit **parameter** — the state, supplied by the runtime | yes, via `:>` |
| `from` | implicit **parameter** — the caller, supplied by the runtime | yes (slots only) |
| `new` | implicit **local** — a copy derived from `this` | **no** — nothing supplies it |

`new` is not a parameter. It is a local the block derives from `this`, so it
never appears in a type, in `make` blocks or in slots. `this` and `from` are
genuine parameters that simply are not written.

This is also the cleanest argument for annotating slots with a distinct sigil
(see "Annotating slots" below): `:>` omits one implicit parameter, a slot
signature omits two.

`from` is typed `From<T>` by the slot's declared reply type:

```kex
foul slot search(q: String) -> Reply<[Hit]> do   # from : From<[Hit]>
  Task.start do
    from.reply(Db.search(q))    # checked against [Hit]
  end
  return { new }
end
```

The deferred reply is therefore checked exactly as an immediate one is — already
stronger than `GenServer.reply(From, anything)`.

**`From<T>` is not a `Process`.** In Erlang `From` is `{Pid, Ref}` — the
caller's pid *plus a unique reference* that tags the reply. The reference is
essential: a caller may have several calls outstanding, and `Ref` routes each
reply to the right waiting `receive`. Sending to the bare pid means the caller
never matches the message and the call times out.

```kex
from.reply(value)     # checked against T
from.pid              # the caller's Pid, when you need it
```

(The type is `Pid`; `Process` is the module — `Process.self : Pid`, `make Pid`.)

`from.pid` is worth exposing — for logging, or for monitoring the caller so
queued work can be dropped if they die (the connection pool wants exactly this:
a waiter that dies should not be handed a connection). The reverse is
impossible: a `From<T>` cannot be built from a `Pid`, because there is no
reference to invent.

**Why a bare `Pid` is insufficient** — for correctness, not convention:

- **Multiple outstanding calls.** One process calling the same server twice has
  two replies in flight. With only a pid to match on, the caller cannot tell
  which reply belongs to which call, and they can be swapped.
- **Late replies after a timeout.** A call times out, the caller moves on, and
  the reply then arrives — indistinguishable from the reply to the *next* call.
  The `Ref` marks it stale so it can be discarded; gen_server depends on this.

Conforming to `gen_server` also settles it independently: `$gen_call` specifies
`{Pid, Ref}`, so a bare pid would mean not conforming.

Being plain data (`{Pid, Ref}`) is also what makes `waiting : [From<Conn>]`
storable in server state.

Rules that follow:

- **Casts have no `from`.** Nobody is waiting, so referencing it there is a
  compile error — worded to say why: *"`from` is not available in a cast — no
  caller is waiting for a reply."*

  Users mostly do not need to know this in advance; the error teaches it. The
  one case worth documenting is the natural question **"who sent me this
  cast?"**, which is legitimate (logging, attribution, dropping work from dead
  callers) and which `from.pid` cannot answer. The information does not exist:
  `gen_server:cast(Pid, Msg)` carries no sender and `handle_cast(Msg, State)`
  has nowhere to get one. The answer is the ordinary Erlang convention — the
  caller includes itself:

  ```kex
  slot enqueue(job: Job, by: Pid) do ... end    # caller passes Process.self
  ```

  Conformance settles this rather than taste: Kex generates both ends and could
  smuggle a sender into cast messages, but then an Erlang caller doing
  `gen_server:cast(Pid, {enqueue, Job})` would send something the Kex server
  could not handle.
- **It escapes into closures safely.** `From` is plain data (`{Pid, Ref}`), so
  capturing it in a `Task.start` block and replying from another process needs
  no special handling on BEAM.
- **Deferred reply implies `foul`** — `from.reply(...)` is a send.
- **Forgetting to reply is the real hazard.** A slot declared `-> Reply<T>` that
  returns `{ new }` promises a later reply; never sending it hangs the caller
  until timeout. gen_server has this hole too. Close most of it cheaply:
  **require that `from` appears in the body of any call slot that returns
  without `reply:`**. It misses "used only in a branch that did not run", but it
  catches forgetting entirely — turning a production hang into a compile error.
- **Replying twice** is left as a runtime no-op or warning. Preventing it
  statically needs linearity; gen_server permits it silently.

This is not optional: any server touching IO needs it, or the whole application
serialises behind its slowest query.

### `make X` and `serving X` together


Both may exist for one type. The receivers differ, so calls are unambiguous:

```kex
counter.increment(3)     # counter : Counter        → make method, local, pure
server.increment(3)      # server : Server<Counter> → slot, remote, blocking
```

This is the recommended layering — domain logic in `make`, protocol surface in
`serving`:

```kex
record Counter do count : Integer = 0 end

make Counter do                                   # pure, testable without a process
  let bumped(by: Int) -> Counter = New { count: @count + by }
end

serving Counter do                                # thin protocol wrapper
  slot increment(by: Int) -> Reply<Int> do
    let next = @bumped(by)
    return { new: next, reply: next.count }
  end
end
```

Better separation than gen_server encourages, where logic tends to accumulate
inside `handle_call` for want of anywhere else to put it.

**A name may not be both a `slot` and a `let` in the same `serving` block** — a
bare `name :> T` annotation could not say which it belongs to, and neither could
a caller inside the block.

The last row is the honest limit: a `Server<T>` obtained from state or an
argument cannot be proven distinct from self, so mutual and self deadlock via
arbitrary handles stay possible. The rule catches the syntactic cases, which are
the ones people actually write.

**IO in a slot is `foul slot` — but blocking is the real problem.** While a slot
waits on a database, the server processes nothing else; every message queues
behind it. That is the standard gen_server bottleneck.

OTP's answer is replying later: return `{:noreply, State}`, do the work
elsewhere, then `GenServer.reply(From, Result)`. Kex needs the same, which means
exposing the caller handle:

```kex
foul slot search(q: String) -> Reply<[Hit]> do
  Task.start do
    from.reply(Db.search(q))     # replies after the slot returned
  end
  return { new }                 # no `reply:` — caller still waiting
end
```

**Consequence for the shape:** `{ new }` with no `reply:` currently reads as a
cast. For a slot declared `-> Reply<T>` it must instead mean *"replying later"*.
The two are distinguishable by the declared return type, but the rule has to be
stated rather than inferred.

### Dependencies of the chosen shape


Three, all small and all useful beyond this feature:

- **Brace shorthand.** `{ new, reply: ... }` needs `{ x }` ⇒ `{ x: x }`.
  Verified missing today: `{ count, other: 1 }` is a syntax error at the comma.
- **Record type inference for brace literals.** `{ reply: @count }` must
  construct a `Reply<Int>` from context. Currently a **bug** — `let a: V = { x: 1.0 }`
  typechecks clean and yields a **Map** (see `todos.txt`).
- **`Reply<T>` state field defaults to unchanged**, so read-only slots can omit
  it. Falls out of record field defaults plus the completeness rule.

Note the syntax uses angle brackets: `Reply<Int>` is the type; `Reply(Int)`
parses as a *constructor call*, since `type Reply<T> = Reply(T)` is a generic ADT.

### Interop


Three separate obligations; the first two are what the bug fixes buy:

1. **Inbound raw.** A Kex process must receive terms sent by Erlang/Elixir, and
   OTP system messages (`DOWN`, `EXIT`, `nodedown`, timeouts). This requires
   `receive` to match raw terms rather than a private envelope.
2. **Outbound raw.** `send` must transmit the term as written, so Erlang code
   receives what it expects.
3. **Protocol conformance.** For a Kex server to be *callable* from Erlang,
   supervisable by an OTP supervisor, and visible to `:observer`, the generated
   code should speak the standard `$gen_call` / `$gen_cast` protocol rather than
   a private tag. Both ends are generated, so conforming costs nothing at the
   point of use — it is purely a choice about what is emitted.

Note that (3) supersedes the `{:increment, 12, self}` shape sketched above: the
sender-in-message idea is right, but `$gen_call` already specifies exactly that,
and using it buys OTP compatibility for free.

#### Matrix

| Direction | Status |
|---|---|
| Kex → Erlang **functions** | ✅ **works today** — `Erlang.*` maps to BEAM modules (`Erlang.Lists.reverse`, `Erlang.Erlang.abs`); see `examples/erlang_interop.kex` |
| Kex ← raw **messages** | ❌ blocked by the `{'kex_msg', ...}` envelope — stage A |
| Kex → Erlang **gen_server** | needs `$gen_call` sending; typed version needs foreign slot declarations |
| Erlang → Kex **server** | free *if* generated servers speak `$gen_call` |
| Kex under an **OTP supervisor** | needs `start_link` / `init` conventions |

The last three collapse into one decision: **conform to `gen_server` rather than
inventing a protocol.** If generated servers implement `init/1`,
`handle_call/3`, `handle_cast/2` and `start_link`, Erlang can call them, OTP
supervisors can supervise them, and `:observer` can see them — at no cost to the
Kex call site, since both ends are generated.

For calling an *existing* Erlang gen_server with checking, the natural shape is
a foreign declaration — a header with no bodies:

```kex
serving external :config_server do
  slot get(key: Atom) -> Reply<Term>
  slot put(key: Atom, value: Term) -> Reply<Void>
end
```

Unverifiable by construction (the Erlang side may disagree), but it moves the
failure from "wrong tag at runtime" to "wrong against a reviewable declaration",
which is how every FFI works. Out of scope for a first cut; the design should
not preclude it.

### `receive` after the envelope goes


Two forms are needed: one that binds a sender and one that does not. **The
default is the interoperable one**, so the average library is correct by
default. Sender-bearing generic messages use the ordinary Erlang convention
`{SenderPid, Payload}` rather than a Kex-specific tag:

```kex
receive do ... end               # raw terms — sees DOWN/EXIT and Erlang senders
pid.sendFrom(message)            # emits {Process.self, message}
receive do |sender| ... end      # matches {SenderPid, Payload}
```

Plain `send` emits exactly its argument. A sender-binding receive additionally
checks that the first tuple element is a pid. Raw messaging and this
sender-bearing form must land atomically so sender-aware programs have no
broken intermediate revision.

A process that handles both application messages and OTP system messages uses
one raw receive and ordinary tuple patterns; it does not need two receives:

```kex
receive do
  (sender, :ping) => sender.send(:pong)
  (DOWN, ref, :process, pid, reason) => handleDown(ref, pid, reason)
  (EXIT, pid, reason) => handleExit(pid, reason)
end
```

`receive do |sender|` remains concise sugar for loops whose relevant messages
all follow `{SenderPid, Payload}`. The raw form is the interoperable foundation
and can mix sender-bearing tuples, OTP notifications, and arbitrary foreign
terms clause by clause.

### Worked examples


`Counter` is the smallest thing that shows the shape; these are closer to what a
framework actually runs. Stdlib calls are illustrative.

**Session store** — the common case: a map behind a process, mixed reads,
writes and fire-and-forget.

```kex
record SessionStore do
  sessions : { String: Session } = {}
  hits     : Integer = 0
end

serving SessionStore do
  # read: no state change, so `new` is omitted
  slot fetch(id: String) -> Reply<Session?> = { reply: @sessions.get(id) }

  # write: replies with what it stored
  slot put(id: String, session: Session) -> Reply<Session> do
    new.sessions = @sessions.put(id, session)
    return { new, reply: session }
  end

  # read AND write — the case a plain accessor cannot express
  slot touch(id: String) -> Reply<Result<Session, NotFound>> do
    return match @sessions.get(id) do
      Just(s) => do
        new.sessions = @sessions.put(id, s.refreshed)
        new.hits = @hits + 1
        { new, reply: Ok(s.refreshed) }
      end
      None => { new, reply: Error(NotFound) }
    end
  end

  # cast: no reply type, so the caller does not block
  slot delete(id: String) do
    new.sessions = @sessions.delete(id)
  end
end
```

Note what the checker gets from this that Elixir cannot: `store.fetch(42)` is a
compile error (wrong argument type), `store.fetc("x")` is a compile error
(no such slot), and `store.fetch("x")` is known to answer `Session?`, so
forgetting to handle `None` is caught too.

**Rate limiter** — realistic middleware, and shows a slot whose reply drives
control flow at the call site.

```kex
record RateLimiter do
  perMinute : Integer
  counts    : { String: Integer } = {}
end

serving RateLimiter do
  slot allow?(key: String) -> Reply<Bool> do
    let used = @counts.get(key).or(0)
    return { reply: false } if used >= @perMinute
    new.counts = @counts.put(key, used + 1)
    return { new, reply: true }
  end

  slot reset do
    new.counts = {}
  end
end

# in a handler
let limiter = Process.spawn(RateLimiter { perMinute: 100 })
...
return Response.textWithStatus("rate limited", 429) if !limiter.allow?(clientIp)
```

The early `return { reply: false }` is worth noticing: it declines to change
state simply by omitting `new`, with no separate "don't update" construct.

**Job queue** — list state, and a cast for the hot path.

```kex
record JobQueue do
  pending : [Job] = []
  done    : Integer = 0
end

serving JobQueue do
  slot enqueue(job: Job) do              # cast — callers should not block
    new.pending = @pending + [job]
  end

  slot take -> Reply<Job?> do
    return match @pending do
      []           => { reply: None }
      [next | rest] => do
        new.pending = rest
        new.done = @done + 1
        { new, reply: Just(next) }
      end
    end
  end

  slot depth -> Reply<Integer> = { reply: @pending.count }
end
```

#### Harder shapes — deferred replies, termination, interop

**Connection pool** — the flagship case for deferred replies. `checkout` cannot
always answer immediately, so callers are parked and answered later. Note that
`from` is *stored in state*: `From<T>` is plain data, which is what makes this
work.

```kex
record ConnPool do
  idle    : [Conn]       = []
  waiting : [From<Conn>] = []
end

serving ConnPool do
  slot checkout -> Reply<Conn> do
    return match @idle do
      [c | rest] => do
        new.idle = rest
        { new, reply: c }            # a connection was free — answer now
      end
      [] => do
        new.waiting = @waiting + [from]
        { new }                      # none free — park the caller, reply later
      end
    end
  end

  slot checkin(c: Conn) do           # cast — the returner should not block
    return match @waiting do
      [w | rest] => do
        w.reply(c)                   # hand straight to the longest waiter
        new.waiting = rest
        { new }
      end
      [] => do
        new.idle = @idle + [c]
        { new }
      end
    end
  end

  slot stats -> Reply<(Integer, Integer)> = { reply: (@idle.count, @waiting.count) }
end
```

Every caller of `checkout` sees one signature and blocks once; whether it was
answered immediately or minutes later is invisible to them. In gen_server this
is the same shape, but `From` is untyped and nothing checks that
`GenServer.reply(w, c)` sends a `Conn`.

**Cache with conditional deferral** — fast path replies immediately, slow path
defers. This is the pattern that ruled out a `Deferred<T>` return marker: one
slot needs both.

```kex
record UserCache do
  entries : { Integer: User } = {}
  misses  : Integer = 0
end

serving UserCache do
  foul slot fetch(id: Integer) -> Reply<Result<User, DbError>> do
    return match @entries.get(id) do
      Just(u) => { reply: Ok(u) }            # hit — immediate, no state change
      None    => do
        new.misses = @misses + 1
        Task.start do
          from.reply(Db.loadUser(id))        # miss — reply after the query
        end
        { new }
      end
    end
  end

  slot put(id: Integer, u: User) do          # cast
    new.entries = @entries.put(id, u)
  end

  slot invalidate(id: Integer) do
    new.entries = @entries.delete(id)
  end
end
```

The server keeps serving other messages while `Db.loadUser` runs — which is the
entire reason deferred replies exist. Without them this slot would serialise
every request in the application behind one database round trip.

**Transient worker that terminates** — `stop:` in practice. A migration runner
that exits when its work is done rather than being supervised forever.

```kex
record Migration do
  steps : [Step]
  done  : Integer = 0
end

serving Migration do
  foul slot runNext -> Reply<Result<Integer, MigrationError>> do
    return match @steps do
      [] => { stop: :normal, reply: Ok(@done) }     # finished — reply, then exit
      [s | rest] => match @apply(s) do
        Ok(_)  => do
          new.steps = rest
          new.done  = @done + 1
          { new, reply: Ok(new.done) }
        end
        Error(e) => { stop: :failed, reply: Error(e) }   # abort the run
      end
    end
  end

  foul let apply(s: Step) -> Result<Void, MigrationError> = Db.exec(s.sql)
end
```

`apply` is a `let`, not a `slot` — it is internal logic, so it never becomes
part of the wire protocol and cannot be invoked by message.

**Called from Erlang** — because generated servers conform to `gen_server`,
nothing special is needed on either side:

```erlang
{ok, Pid} = kex_conn_pool:start_link(#{}),
Conn      = gen_server:call(Pid, {checkout}),
ok        = gen_server:cast(Pid, {checkin, Conn}),
{I, W}    = gen_server:call(Pid, {stats}).
```

and the same server can sit under an ordinary OTP supervisor child spec.

## Stages

- **A — interoperable messaging.** `send` transmits raw and `receive` matches
  raw. `sendFrom` emits the conventional `{SenderPid, Payload}` pair and
  `receive do |sender|` matches it. This fixes both live bugs and atomically
  migrates existing sender-bearing uses and specs.
- **B — `Server<X>` and `Process.spawn`.** The typed handle plus the foul
  constructor, alongside the existing `Task`.
- **C — `serving` blocks and slots.** Parser, checker (slot signatures, state
  typing), and generation of the typed client stubs via `let %name`. Emits
  `gen_server` callbacks (`init/1`, `handle_call/3`, `handle_cast/2`,
  `start_link`) directly — conformance is part of this stage, not a later
  retrofit, since it decides what the generator emits.
- **D — supervision integration**, once `DOWN`/`EXIT` are receivable: `start_link`
  semantics, restart strategies, child specs.

Stages A-C form the first implementation. They include calls, casts, deferred
replies, stopping, timeout configuration, generated `gen_server` callbacks, and
interpreter/BEAM parity. Foreign typed server declarations, Kex supervision
policy syntax, termination hooks, and stronger exhaustiveness checking are
deferred. Uncaught errors and non-exhaustive clauses crash the server, following
ordinary OTP and current Kex function behaviour. Generated `start_link` still
allows an Erlang supervisor to supervise a Kex server before stage D.

Stage A is worth doing immediately regardless: it fixes a memory leak and
unblocks `monitor`/`link`, which are currently shipped-but-broken.

## Files

- `runtime/src/kex_intrinsic_process.erl` — `send`, plus spawn/gen_server support
- `src/ir/lower.cxx` (~1406), `src/ir/emit_core.cxx` (~281), `src/ir/ir.hxx` (~170)
- `src/stdlib/process.kex` — `Process.spawn`, `Server<X>`
- `src/parser/parser.cxx`, `src/ast/ast.hxx` — `serving` / `slot`
- `src/semantic/typechecker.cxx` — slot signatures, `Server<X>` slot lookup
- `grammar.ebnf`
- `spec/` — plus BEAM parity via `make spec-beam`

## Verification

- A regression spec that **monitors a process, kills it, and receives the
  `DOWN`** — currently impossible, and the direct test for bug 1.
- A spec asserting an unmatched foreign message does not accumulate (bug 2).
- A Kex process exchanging messages with a hand-written Erlang module, both
  directions.
- Slot type errors are compile errors: unknown slot, wrong argument type, wrong
  reply usage.
- Interpreter/BEAM parity on all of the above.

## Open questions

1. **Blocking and failure**: resolved — `-> Reply<T>` blocks, a cast does not;
   a call returns `Result<T, CallError>` and `.try` opts into crashing.
   Timeout spelling is resolved: `server.within(ms)` configures repeated calls
   and `slotCall(..., within: ms)` overrides one call. See "Blocking, failure
   and timeouts" in Design.
2. ~~Reply versus `This`~~ — **decided**: slots return both, as
   `-> Reply<T>` with `{ new, reply: ... }`. See Design.
3. ~~Slots and `make` methods on the same type~~ — **resolved**: both may
   exist, receivers differ, and slots may call `make` methods directly. See
   Design → "`make X` and `serving X` together".
4. **Calling foreign gen_servers** with typechecked slots.
5. **Supervision** — how `serving` types participate in supervision trees, and
   whether restart strategy is declared on the `serving` block.

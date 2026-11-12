# Processes in Kex

State that outlives a call belongs to a process. Declare a record for the
state and a `serving` block for what the process answers; `Process.spawn`
starts it and returns a handle on which each slot is called like a function.
Coming from Elixir or Erlang: this is a GenServer, and the table further down
maps one onto the other.

```kex
record Counter do
  count : Integer = 0
end

serving Counter do
  # Reply<T>: the caller waits for an answer.
  slot current -> Reply<Integer> = { reply: @count }

  slot add(by: Integer) -> Reply<Integer> do
    new.count = @count + by
    return { new, reply: new.count }
  end

  # Void: the caller sends and moves on.
  slot reset -> Void = New { count: 0 }
end

main do
  let counter = Process.spawn(Counter {})
  counter.add(2)
  let total = counter.add(3).or(0)
  IO.printLine("total ${total}")
  counter.reset
  IO.printLine(counter.current.or(-1))
end
```

## If you know GenServer

A `serving` block is a GenServer: on the BEAM it compiles to an OTP
`gen_server`, and Erlang or Elixir code can call it as one. What changes is
that the protocol is declared and checked. There are no message tuples to
keep in step with the callbacks, and no separate client functions to write:
each `slot` is both the callback and the function a caller uses.

| GenServer | Kex |
|-----------|-----|
| `use GenServer` in a module | `serving Counter do` on a record |
| the state | the record: `@count`, `this` |
| `GenServer.start(Counter, state)` | `Process.spawn(Counter {})` |
| `handle_call` plus its client function | `slot name(args) -> Reply<T>` |
| `handle_cast` plus its client function | `slot name(args) -> Void` |
| `GenServer.call(pid, {:add, 2})` | `counter.add(2)`, answering `T or! CallError` |
| `GenServer.cast(pid, :reset)` | `counter.reset` |
| `{:reply, value, state}` | `{ reply: value }` |
| `{:reply, value, new_state}` | `{ new, reply: value }` |
| `{:noreply, new_state}` | `New { ... }`, or `new` after assigning fields |
| `{:stop, :normal, value, state}` | `{ stop: :normal, reply: value }` |
| a call's timeout argument | `counter.add(2, within: 1000)` |
| an exit on timeout or a dead server | `Error(Timeout)`, `Error(NoProcess)`, `Error(CallFailed)` |
| `GenServer.reply(from, value)` later | `from.reply(value)` in a `foul slot` |

```kex
record Counter do
  count : Integer = 0
end

serving Counter do
  slot current -> Reply<Integer> = { reply: @count }

  slot add(by: Integer) -> Reply<Integer> do
    new.count = @count + by
    return { new, reply: new.count }
  end

  slot reset -> Void = New { count: 0 }

  slot finish -> Reply<Integer> = { stop: :normal, reply: @count }
end

main do
  let counter = Process.spawn(Counter {})
  IO.printLine(counter.add(2, within: 1000).or(0))
  let patient = counter.within(30000)
  IO.printLine(patient.current.or(0))
  IO.printLine(counter.finish.or(-1))
  match counter.current do
    Ok(n)    => IO.printLine(n)
    Error(_) => IO.printLine("gone")
  end
end
```

## A slot that can refuse

A server that validates answers a result, so its caller sees two layers: the
call itself may fail (`CallError`), and the request may be refused. Keep the
decision in a `make` function that answers the next state or the reason, and
let the slot wrap it:

```kex
type BankError = Insufficient(Integer) | Frozen

record Account do
  balance : Integer = 0
  frozen? : Bool = false
end

# The pure step answers the next state, or why there is none.
make Account do
  let withdrawn(amount: Integer) -> This or! BankError do
    return Error(Frozen) if @frozen?
    return Error(Insufficient(@balance)) if amount > @balance
    Ok(New { balance: @balance - amount })
  end

  # Two answers at once are a tuple.
  let deposited(amount: Integer) -> (This, Integer) do
    let total = @balance + amount
    (New { balance: total }, total)
  end
end

serving Account do
  # A slot that can refuse answers a result. In a `match` arm the answer needs
  # `return`: without it the braces are read as an ordinary map.
  slot withdraw(amount: Integer) -> Reply<Integer or! BankError> do
    match this.withdrawn(amount) do
      Ok(account)  => return { new: account, reply: Ok(account.balance) }
      Error(error) => return { reply: Error(error) }
    end
  end

  slot deposit(amount: Integer) -> Reply<Integer> do
    let (account, total) = this.deposited(amount)
    return { new: account, reply: total }
  end

  slot freeze -> Void = New { frozen?: true }
end

# A parameter list too long for the line breaks one parameter per line.
foul transfer(
  from: Server<Account>,
  to: Server<Account>,
  amount: Integer
) -> String do
  # The caller sees two layers: the call may fail, and so may the withdrawal.
  match from.withdraw(amount) do
    Ok(Ok(left))                => do
      to.deposit(amount)
      "moved ${amount}, ${left} left"
    end
    Ok(Error(Insufficient(has))) => "only ${has} available"
    Ok(Error(Frozen))            => "account frozen"
    Error(_)                     => "account unreachable"
  end
end

main do
  let ada = Process.spawn(Account { balance: 100 })
  let bob = Process.spawn(Account {})
  IO.printLine(transfer(ada, bob, 30))
  IO.printLine(transfer(ada, bob, 500))
  ada.freeze
  IO.printLine(transfer(ada, bob, 1))
  IO.printLine(bob.deposit(0).or(-1))
end
```

## Slots with several clauses, and `::>`

A slot with one body carries its types in the `slot` line, as a function does
in its `let`. A slot written as several pattern-matching clauses takes a
declaration above them, and `serving` has a separator of its own for it:

- `name ::> Args -> Reply<T>` declares a call. Both the receiver and the
  caller, `from`, are implied.
- `name :> Args -> Void` declares a cast. Only the receiver is implied, as in
  a `make`; nobody is waiting, so there is no `from`.

```kex
type Command = Increment(Integer) | Current

type Signal = Reset | Bump

record Tally do
  count : Integer = 0
end

serving Tally do
  # A call: `::>`.
  apply ::> Command -> Reply<Integer>
  slot apply(Increment(by)) do
    new.count = @count + by
    return { new, reply: new.count }
  end
  slot apply(Current) = { reply: @count }

  # A cast: `:>`.
  signal :> Signal -> Void
  slot signal(Reset) = New { count: 0 }
  slot signal(Bump)  = New { count: @count + 1 }
end

main do
  let tally = Process.spawn(Tally {})
  IO.printLine(tally.apply(Increment(5)).or(0))
  tally.signal(Bump)
  IO.printLine(tally.apply(Current).or(0))
  tally.signal(Reset)
  IO.printLine(tally.apply(Current).or(-1))
end
```

## `make` and `serving` on one record

A record can have both. The `make` holds the logic as plain functions, which
can be called and tested without a process; the `serving` block calls them.

```kex
record Account do
  owner   : String
  balance : Integer = 0
  history : [Integer] = []
end

# `make`: plain functions on an Account value.
make Account do
  # `new` is a copy of the receiver whose fields can be assigned, for a
  # change that touches several of them.
  let deposited(amount: Integer) -> This do
    new.balance = @balance + amount
    new.history = [amount | @history]
    new
  end

  let rich? -> Bool = @balance > 100
end

# `serving`: what a process holding an Account answers. The same names work:
# `@balance`, `this`, `New`, `new`, and the functions from the `make` above.
serving Account do
  slot balance -> Reply<Integer> = { reply: @balance }

  slot deposit(amount: Integer) -> Reply<Integer> do
    let updated = this.deposited(amount)
    return { new: updated, reply: updated.balance }
  end

  slot rename(owner: String) -> Void = New { owner: owner }

  # A slot and a function on the same record cannot share a name.
  slot wealthy? -> Reply<Bool> = { reply: this.rich? }
end

main do
  let local = Account { owner: "Ada" }.deposited(50)
  IO.printLine("${local.balance} ${local.history}")

  let account = Process.spawn(Account { owner: "Ada" })
  account.deposit(70)
  IO.printLine(account.deposit(70).or(0))
  account.rename("Grace")
  IO.printLine(account.wealthy?.or(false))
end
```

## Rules

- A slot's return type is its protocol: `Reply<T>` for a call that answers
  (`handle_call`), `Void` for a message nobody waits on (`handle_cast`).
- The braces are a slot's answer only where the slot ends: after `=`, as the
  last expression of its body, or after `return`. In a `match` arm or an `if`
  branch write `return { ... }`; without it the braces are an ordinary map
  and the checker reports a type mismatch.
- Never build the answer as a record (`Reply { new: ..., reply: ... }`) and
  never have a helper function return one: a `make` function answers the
  next state, and the slot writes the braces itself.
- A slot that can refuse is `Reply<T or! E>`, and its caller gets
  `(T or! E) or! CallError`: match `Ok(Ok(value))`, `Ok(Error(reason))` and
  `Error(_)`.
- Match a failed call as `Error(_)` unless the reason matters.
- A slot answers with one of: `{ reply: value }` to answer and keep the state,
  `{ new, reply: value }` to install the changed state and answer, or the new
  record itself (`New { ... }`, or `new` after assigning its fields) to change
  state without answering. Adding `stop: :normal` ends the process.
- Inside a slot `@count` reads the current state and `new.count = ...` sets a
  field on the copy that becomes the next state. `this`, `This` and
  `New { ... }` mean what they mean in a `make`.
- A slot and a function in a `make` for the same record cannot share a name.
- Do not put a declaration above a slot that has one body. For a slot with
  several clauses, declare it with `::>` when it answers `Reply<T>` and `:>`
  when it answers `Void`.
- A call to a `Reply<T>` slot answers `T or! CallError`, because the process
  may be gone or slow: finish it with `.or(default)`, `.try` or `match`. A
  call waits five seconds unless told otherwise: `counter.add(2, within: 1000)`
  for one call, `counter.within(30000)` for a handle that always waits longer.
- Calling a slot is an effect: only `main` and `foul` functions may do it.
- Prefer `serving` to hand-written `send` and `receive`. `kex -s Process`
  lists the lower-level functions.

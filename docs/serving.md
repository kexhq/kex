# Typed Servers with `serving`

`serving` attaches a typed request protocol to a record. `Process.spawn(state)`
owns that state in a server process and returns a `Server<State>` handle.

```kex
record ShoppingList do
  items : [String] = []
end

serving ShoppingList do
  slot items -> Reply<[String]> = { reply: @items }

  slot add(item: String) -> Reply<Integer> do
    new.items = [item | @items]
    return { new, reply: new.items.count }
  end

  slot clear -> Void = New { items: [] }
end
```

A slot returning `Reply<T>` is a synchronous call. A slot returning `Void` is
an asynchronous cast. Calling the slot on a `Server<ShoppingList>` checks its
name and arguments at compile time:

```kex
let groceries = Process.spawn(ShoppingList {})
groceries.add("coffee")              # Result<Integer, CallError>
groceries.clear()                     # enqueued; returns immediately
```

Calls are foul because they send and wait. Casts are foul because they send.
Mailbox ordering guarantees that a following call observes a preceding cast
sent by the same process.

## Slot signatures are not duplicated

The inline declaration is sufficient:

```kex
slot items -> Reply<[String]> = { reply: @items }
```

For multi-clause slots, a separate annotation may be clearer:

```kex
apply ::> Command -> Reply<Integer>
slot apply(Increment(by)) = { reply: @count + by }
slot apply(Current) = { reply: @count }
```

The annotation is optional. If both forms state a return type, they must agree.
Call annotations use `::>` because the handler has a typed caller reference;
cast annotations use `:>` and cannot access `from`.

## State and stop transitions

Slot results describe state and reply together:

- `{ reply: value }` replies without changing state.
- `{ new, reply: value }` installs the implicit updated record and replies.
- `{ new }` installs state without an immediate reply.
- adding `stop: reason` terminates after the transition; a call reply is sent
  before termination.

The `new` and `New` forms are the ordinary functional record-update operations
described in [Records and Functional Updates](records-and-updates.md).

## Timeouts

Calls use the first applicable timeout:

1. a per-call `within:` argument;
2. the immutable handle returned by `server.within(milliseconds)`;
3. 5000 milliseconds.

```kex
groceries.add("coffee", within: 10_000)
let patient = groceries.within(30_000)
patient.items()
```

Timeouts return `Error(Timeout)`; stopped or failed servers use the other
`CallError` cases.

## Deferred replies

A foul call slot may capture its implicit `from : From<T>` and reply later:

```kex
foul slot lookup(key: String) -> Reply<Value> do
  Task.start { from.reply(load(key)) }
  return { new }
end
```

`from` contains both the caller pid and the unique call reference. This keeps
multiple outstanding calls distinct. A call slot returning without `reply:`
must use `from`; casts have no caller and cannot use it.

## Erlang and Elixir interoperability

On BEAM, a Kex server is an OTP `gen_server`. Slots use ordinary tuple request
terms, so foreign callers can send `{add, <<"coffee">>}` with
`gen_server:call/2` or the atom `clear` with `gen_server:cast/2`. Only declared
slots are exposed; ordinary helper methods are not request handlers.

The full rationale, rejected alternatives, and deferred supervision work live
in [the serving design plan](serving-plan.md).

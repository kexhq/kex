# Concurrency

## Process Model

Kex uses an Elixir-style process model with lightweight, isolated processes communicating via message passing.

> **Status.** Process primitives and typed `Process<Message>` handles run on
> both the interpreter and BEAM. Typed record-backed servers are documented in
> [Typed Servers](serving.md). The richer supervision DSL shown below remains
> forward-looking.

## Spawning Processes

```kex
foul pid = spawn do
  loop do
    receive do
      (:ping, sender) => sender.send(:pong)
    end
  end
end
```

## Typed Processes

Processes declare what messages they accept:

```kex
type CounterMessage = :increment | :reset | (:get, Process<Int>)

foul counter: Process<CounterMessage> = spawn do
  var state = 0
  loop do
    receive do
      :increment => state = state + 1
      :reset => state = 0
      (:get, sender) => sender.send(state)
    end
  end
end

counter.send(:increment)     # ok
counter.send("hello")        # compile error
```

Escape hatch for dynamic messaging:

```kex
let dynamic: Process<Any> = spawn do ... end
```

## Two Styles

Imperative (var + loop):

```kex
foul counter = spawn do
  var state = 0
  loop do
    receive do
      :increment => state = state + 1
    end
  end
end
```

Functional (recursive):

```kex
foul counter = spawn do
  let loop(state: Int) do
    receive do
      :increment => loop(state + 1)
      (:get, sender) => do
        sender.send(state)
        loop(state)
      end
    end
  end
  loop(0)
end
```

## Tasks

`Task` is a library built on processes — no special keywords:

```kex
main do
  let task1 = Task.start { fetchUser(id) }
  let task2 = Task.start { fetchPosts(id) }

  let user  = task1.await(timeout: 5000)
  let posts = task2.await(timeout: 5000)
  # `await` returns Result<User, TaskError> / Result<[Post], TaskError> —
  # handle with match or flatMap rather than short-circuiting syntax.
end
```

## Supervision

```kex
foul app = Supervisor.start(restart: :only_crashed) do
  worker(Database, args: [config.db_url])
  worker(Cache)
  supervisor(restart: :all) do
    worker(WebServer, args: [config.port])
    worker(WebSocket)
  end
end
```

When a child crashes, the supervisor restarts it based on its restart policy.

## Receive with Timeout

The timeout belongs to the `after` clause it governs:

```kex
receive do
  msg => handle(msg)
after timeout: 5000
  handleTimeout()
end
```

`after` runs when nothing arrived, so it takes no pattern and no arrow, and its
body runs to the receive's own `end` — there is no second block to close.

A receive with no clauses and only an `after` is how a process waits:

```kex
foul sleep(ms: Int) do
  receive do
  after timeout: ms
  end
end
```

`after timeout: 0` polls the mailbox without blocking, and a receive with no
`after` blocks until a message matches.

## Hot Code Reload

On the BEAM, a module can be replaced while it runs, and long-running
processes carry on in the new code. The VM keeps two versions of a module. A
process moves to the newer one only when it makes a fully-qualified call, and
a process still running the older one is killed when a second reload retires
it.

Kex compiles its process loops so that they move:

- A function whose body receives calls this module's functions remotely, so
  a server written as `serve(state)` calling itself from its `receive` arms
  picks up the new `serve` after its next message.
- A `loop do ... end` whose body receives is compiled into a function of its
  own, which it re-enters through a remote call. The `var`s it updates are
  that function's parameters, so they carry across the switch.

Functions that never receive keep their fast local calls.

The message a process was already waiting for when the new code arrived is
handled by the old code. The call it makes afterwards is the one that moves
it. A loop's state has to keep its shape across versions, as in Erlang:
changing which `var`s a receiving loop updates changes the parameters of its
compiled function, and so does renaming the function it lives in.

A loop containing a `return` that leaves the enclosing function stays where it
is, since the `return` has to reach the function that catches it. It keeps
working, but does not move to new code.

`serving` servers reload as well; see [Typed Servers](serving.md).

## Clustering

A node is one running BEAM VM with a name. Start one with `--sname` (or
`--name` for a fully-qualified host name) and a shared `--cookie`, or from
inside the program with `Node.start`:

```sh
kex --sname a --cookie secret server.kex
kex --sname b --cookie secret client.kex
```

Once connected, a `Pid` from one node works on the other. `send`, `link` and
`monitor` cross the network unchanged, and so do typed `Process` handles and
`Server` handles: a slot called on another node's server runs there, against
that server's state.

```kex
using Node

main do
  let a = :a@myhost
  Node.connect(a)
  Node.send(a, :requests, Ping(Process.self, "hi"))
  Node.whereis(a, :requests).map { |pid| pid.send(:flush) }
  Node.spawn(a) do IO.printLine("running on ${Node.self}") end
end
```

Node names are atoms: `:a@myhost` for a short name, `:"a@host.example.com"`
for a long one, or `Atom.from("a@" + host)` for one built at runtime.

Plain data needs nothing on the other side: numbers, strings, lists, tuples
and records arrive as themselves. Anything carrying a function needs the same
compiled code on both nodes, such as a lambda, or a record whose methods the
receiver calls. `Node.spawn` sends its block's compiled module to a node that
has not loaded it. It never replaces a module the other node already has,
since processes there may be running it.

`examples/cluster/` is a complete two-node program: a node that owns a
`serving` key-value store, and a client node that reaches it by name, calls
its slots across the connection and runs a block on it with `Node.spawn`. Its
header comments say how to start the two nodes.

The tree-walk interpreter is always a single, unnamed node: `Node.alive?` is
`false` and `Node.self` is `:nonode@nohost`.

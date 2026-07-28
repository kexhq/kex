# Concurrency

## Process Model

Kex uses an Elixir-style process model with lightweight, isolated processes communicating via message passing.

> **Status.** The process primitives — `spawn`, `receive`, `send`, `loop`, and
> `Task` — are implemented and run on both the interpreter and the BEAM
> backend (see `spec/process_spawn_receive.kex`). The *typed* process surface
> (`Process<Message>` with a compile error on a mismatched `send`) and
> `Supervisor` shown below are the intended design but are **not yet
> enforced/implemented**; treat those sections as forward-looking.

## Spawning Processes

```kex
foul pid = spawn do
  loop do
    receive do
      (:ping, sender) -> sender.send(:pong)
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
      :increment -> state = state + 1
      :reset -> state = 0
      (:get, sender) -> sender.send(state)
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
      :increment -> state = state + 1
    end
  end
end
```

Functional (recursive):

```kex
foul counter = spawn do
  let loop(state: Int) do
    receive do
      :increment -> loop(state + 1)
      (:get, sender) -> do
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
foul do
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

```kex
receive timeout: 5000 do
  msg -> handle(msg)
after -> handleTimeout()
end
```

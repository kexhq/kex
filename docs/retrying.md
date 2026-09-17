# Retrying operations

Import `Control.Retry` to retry an operation that returns `Result`. The block
runs once immediately. An `Ok` stops the run; an `Error` waits according to
the schedule and tries the block again. When the schedule is exhausted, the
last error is returned unchanged.

```kex
using Control.Retry
using Net.HTTP

let result = Retry.run(attempts: 5) do
  HTTP.get("https://api.example.com/inventory")
end
match result do
  Ok(response) => IO.printLine(response.status.code)
  Error(error) => IO.printLine(error.message)
end
```

Five attempts includes the initial call: at most four sleeps and four retries.
There is no delay before the first call or after the final result. Runtime
faults are not converted into returned errors.

HTTP 429 or 503 responses are `Ok(response)`, not `Error(NetError)`. The example
above retries transport errors only. Selecting statuses is shown below.

## A schedule describes timing and limits

Options may be supplied directly to `Retry.run`, or collected in an immutable
`Retry.Schedule`. Constructing a schedule does not start anything.

| Setting | Default | Meaning |
| --- | --- | --- |
| `attempts` | `3` | Total executions, including the first |
| `delay` | `100.milliseconds` | Base wait before the second attempt |
| `backoff` | `2.0` | Multiplier between base waits; `1.0` means fixed delays |
| `maximumDelay` | `5.seconds` | Cap on each actual wait, including jitter |
| `jitter` | `0.2` | Symmetric proportional variation; `0.0` disables it |
| `maximumTotalDelay` | `None` | Optional cap on cumulative actual sleep |

For finite settings, attempts below one become one, negative durations become
zero, multipliers below one become one, and jitter is clamped to `0.0..1.0`.
Float settings must be finite. These adjustments apply when running; they do
not mutate the schedule.

```kex
let schedule = Retry.Schedule {
  attempts: 5,
  delay: 200.milliseconds,
  backoff: 2.0,
  maximumDelay: 5.seconds,
  jitter: 0.0
}

let result = Retry.run(schedule: schedule) do
  HTTP.get("https://api.example.com/inventory")
end
```

If every attempt fails, this schedule has the following waits:

| Attempt | Wait before call | Total scheduled sleep |
| --- | --- | --- |
| 1 | None | 0 ms |
| 2 | 200 ms | 200 ms |
| 3 | 400 ms | 600 ms |
| 4 | 800 ms | 1400 ms |
| 5 | 1600 ms | 3000 ms |

An earlier success stops immediately. Request execution time is additional:
three seconds of scheduled sleep does not mean a three-second request deadline.

## Fixed delays and capped backoff

A fixed-delay schedule waits 250 ms between calls:

```kex
let fixed = Retry.Schedule {
  attempts: 4,
  delay: 250.milliseconds,
  backoff: 1.0,
  maximumDelay: 250.milliseconds,
  jitter: 0.0
}
```

Complete failure means four calls and three waits, totalling 750 ms of sleep.
A capped exponential schedule can wait 500 ms, 1 s, 2 s, 2 s, then 2 s:

```kex
let capped = Retry.Schedule {
  attempts: 6,
  delay: 500.milliseconds,
  backoff: 2.0,
  maximumDelay: 2.seconds,
  jitter: 0.0
}
```

Reaching the delay cap does not stop the run. Subsequent waits remain bounded,
and the attempt limit still applies. The cap applies to the initial wait too.

## Jitter and total sleep

With `jitter: 0.2`, the 200, 400, 800, and 1600 ms base waits range over
160–240, 320–480, 640–960, and 1280–1920 ms. Each wait is sampled independently;
jitter does not change the next base wait or the attempt limit. Actual waits
are capped by `maximumDelay`, including positive jitter.

```kex
let schedule = Retry.Schedule {
  attempts: 5,
  delay: 1.seconds,
  backoff: 2.0,
  jitter: 0.0,
  maximumTotalDelay: Just(2.seconds)
}
```

After an error, this schedule permits a one-second sleep and a second call.
If that call also fails, the next two-second wait would exceed the total
allowance, so the run returns that error without sleeping again. Equality
with the remaining allowance is permitted. Jittered actual sleep counts
against this budget. Configure operation-specific timeouts separately.

## Reuse and override a schedule

```kex
let apiSchedule = Retry.Schedule { attempts: 5 }
let inventory = Retry.run(schedule: apiSchedule) do
  HTTP.get("https://api.example.com/inventory")
end
let orders = Retry.run(schedule: apiSchedule) do
  HTTP.get("https://api.example.com/orders")
end
```

Each call gets five attempts and starts from the initial delay. Exhausting
one run does not consume the other's budget. A schedule is not a rate limiter
or shared timer.

Named options override fields for a single execution:

```kex
let result = Retry.run(schedule: apiSchedule, attempts: 2) do
  HTTP.get("https://api.example.com/health")
end
```

`apiSchedule.attempts` remains five. To disable a schedule's total-sleep bound
for one run, explicitly pass `maximumTotalDelay: None`.

## Decide explicitly when an outcome needs another attempt

Use `Retry.again(value)` and `Retry.done(value)` when an operation's
`Ok`/`Error` distinction is not enough. The return value keeps the decision:

- `Retry.Done(value)` means the operation deliberately stopped.
- `Retry.Again(value)` means the schedule was exhausted while another attempt
  was requested. The last supplied value is preserved.

`done` can stop with an error. `again` never bypasses schedule limits. Use one
form consistently: a block returns either ordinary Results or Decisions.

```kex
using Net

let outcome = Retry.run(schedule: apiSchedule) do
  let result = HTTP.get("https://api.example.com/inventory")
  match result do
    Error(error) => if error.kind == Timeout || error.kind == Connect
      Retry.again(result)
    else
      Retry.done(result)
    end
    Ok(response) => if [429, 502, 503, 504].contains?(response.status.code)
      Retry.again(result)
    else
      Retry.done(result)
    end
  end
end

match outcome do
  Done(Ok(response)) => IO.printLine(response.status.code)
  Done(Error(error)) => IO.printLine("stopped: ${error.message}")
  Again(last) => IO.printLine("retry budget exhausted: ${last}")
end
```

This example retries timeouts, connection errors, and four selected statuses.
Other responses stop immediately; `Done(Ok(response))` means an HTTP response
was accepted by the retry rule, not necessarily a 2xx response. Application
code still needs to handle its status. Classification and `Retry-After`
handling belong to the application; the generic runner has no HTTP policy.
Repeating writes requires application-level safety, such as an idempotency key.

[The runnable example](../examples/retrying/requests.kex) simulates a timeout, a 503, and
then a successful inventory response using real HTTP response/error values.
It prints waits instead of sleeping and requires no network access.

## Report retry progress

An optional `onRetry` callback can update a user interface or write a log:

```kex
let result = Retry.run(attempts: 5, onRetry: { |info|
  let progress = "retrying ${info.nextAttempt}/${info.maximumAttempts}"
  IO.printLine("${progress} in ${info.delay.seconds} seconds")
}) do
  HTTP.get("https://api.example.com/inventory")
end
```

The callback runs before sleeping, only when another attempt is permitted.
It is not called for the first attempt, success, `done`, or exhaustion. The
operation does not need extra parameters or its own counter.

| `Retry.Info<X>` field | Meaning |
| --- | --- |
| `attempt` | Number of the attempt that just finished, starting at 1 |
| `nextAttempt` | Number of the upcoming attempt |
| `maximumAttempts` | Total execution allowance |
| `remainingAttempts` | Remaining executions, including the upcoming one |
| `delay` | Upcoming actual sleep after jitter and capping |
| `totalDelay` | Sleep already performed, excluding the upcoming wait |
| `result` | Last `Error` or `Again`, preserving the application's value |

The same hook works with automatic and explicit decisions. It observes the
retry rather than deciding whether to retry. Callback execution time is not
part of the sleep budget; callback faults propagate normally.

## Test without waiting

Override only what a test needs. Replacing sleep keeps the default random
source; replacing randomness keeps real sleep. Production randomness comes
from the backend's secure source.

```kex
let result = Retry.run(
  attempts: 2,
  delay: 4.seconds,
  jitter: 0.25,
  sleeper: { |wait| Assert.equal(wait, 3.seconds) },
  random: { 0.0 }
) do
  Error("temporary")
end
Assert.equal(result, Error("temporary"))
```

The block runs twice, the fake sleeper runs once, and no real sleep occurs.
A sample of `0.0` chooses the lower jitter bound, `0.5` the base wait, and
`1.0` the upper bound before capping. Samples outside the range are clamped.
Zero jitter needs no random sample.

The [retry specs](../spec/stdlib/retry.spec.kex) assert complete call/wait
sequences, recovery, exhaustion, schedule reuse, and HTTP classification.

## Replacing the former API

`Policy`, `fixed`, `exponential`, builder methods, predicates, `runWith`, and
`runWithRandom` are removed. Use `Schedule` fields or named `run` options,
`backoff: 1.0` for fixed delays, and `sleeper:` / `random:` for testing.
`maximumTotalDelay` names the cumulative-sleep limit explicitly. Express custom
classification inside the block with `again` / `done`.

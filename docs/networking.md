# Networking

Kex keeps networking outside the prelude. Import only the protocol modules a
program uses:

```kex
using URI
using Net.IP
using Net.DNS
using Net.Socket
using Net.HTTP
using Net.HTTP.WebSocket
```

The BEAM backend provides the real networking implementations described here.
The tree-walking interpreter keeps pure values—URI, IP, headers, routing and
retry policy logic—available, but real I/O returns a typed
`UnsupportedBackend` error. Check `Net.Support.current` when a program can run
on several backends; a capability is available only when its `usable?` field is
true.

All network failures use `NetError`. Match its stable `kind` and `operation`
fields rather than backend diagnostic text. HTTP 4xx and 5xx statuses are
ordinary successful HTTP exchanges, not transport errors. Handles are opaque,
safe to pass between Kex processes, explicitly closeable, and idempotent to
close.

## URI, addresses, and headers

URI parsing is strict and ASCII-only. Unicode input crosses an explicit IRI
boundary:

```kex
using URI

let base = URL.parse("https://Example.COM:443/api/").try
let reference = URI.parse("../people?q=Ada%20Lovelace").try
IO.printLine(base.resolve(reference).try.normalize.string)

let iri = URI.fromIRI("https://münich.example/straße").try
IO.printLine(iri.host.try.ascii) # xn--mnich-kva.example
```

Generic query encoding uses `%20`; form encoding uses `+`. Both preserve entry
order and duplicates. `Headers` also preserves order, duplicates, and original
name spelling while lookup is case-insensitive. Invalid names, control bytes,
ports, statuses, IP addresses, CIDRs, and DNS names are rejected at their
validated constructors.

## TCP, UDP, Unix streams, and TLS

TCP and Unix streams are byte-oriented. Bounded chunk reads retain unread bytes
for the next operation; exact and line reads return typed limit, timeout, EOF,
or closed-handle errors.

```kex
using Net.Socket

let endpoint = TCP.Endpoint.host("example.com", Net.Port.from(80).try)
let connection = TCP.connect(endpoint, TCP.ConnectOptions {
  connectTimeout: 10.seconds,
  noDelay?: true,
  keepAlive?: true
}).try
connection.sendAll("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n".to(Binary).try).try
let statusLine = connection.receiveLine(8192, 30.seconds).try.to(String).try
IO.printLine(statusLine)
connection.close
```

Listening on port zero asks the operating system for an ephemeral port;
`listener.localAddress.try` reports the assigned endpoint. UDP preserves
datagram boundaries. If an incoming datagram exceeds `receiveFrom`'s limit, it
is consumed and returned as a typed `Limit` error—never silently truncated.
UDP bind policy is explicit: `UDP.BindOptions` controls broadcast permission,
multicast TTL and loopback, and the per-receive timeout. IPv4 multicast groups
are joined and left with an explicit local interface; attempting to join a
unicast group returns `UnsupportedOption`.

```kex
let socket = UDP.bind(
  UDP.Endpoint.any(Net.Port.from(5353).try),
  UDP.BindOptions {
    broadcast?: true,
    multicastTtl: 4,
    receiveTimeout: 2.seconds
  }
).try
socket.joinMulticast(
  Net.IP.Address.parse("239.255.0.1").try,
  Net.IP.Address.parse("0.0.0.0").try
).try
```

Unix addresses must be nonempty absolute filesystem paths. Listening never
removes an existing path implicitly; closing a listener unlinks the path that
listener successfully created.

```kex
let address = Unix.Address.path("/tmp/my-service.sock").try
let listener = Unix.listen(address, Unix.ListenOptions {
  removeStale?: true,
  acceptTimeout: 2.seconds,
  receiveTimeout: 100.milliseconds
}).try
```

`removeStale?` removes only an existing filesystem socket. Regular files,
directories, and other path types are preserved and return `UnsupportedOption`.

Direct TLS clients enable TLS 1.2 and 1.3. Certificate and hostname
verification use system trust by default; disabling verification requires
`ClientConfig { verify?: false }`. The current TLS API is a client byte-stream
subset; server identities, TCP-to-TLS upgrade, mTLS, and negotiated-session
inspection remain design work tracked in [net-plan.md](net-plan.md).

## DNS

An explicit resolver owns a bounded positive and negative cache:

```kex
using Net.DNS

let resolver = Resolver.system(CacheOptions {
  entries: 1024,
  maximumTtl: 1.hours,
  negativeTtl: 30.seconds
}).try
let name = Name.parse("localhost").try
let addresses = resolver.addresses(name).try
IO.printLine("${addresses.length} addresses")
IO.printLine("${resolver.statistics.hits} cache hits")
resolver.close
```

`lookup` supports A, AAAA, CNAME, MX, TXT, SRV, and PTR records. Kex reports
the resolver's DNSSEC state but does not independently validate DNSSEC; the
system resolver currently reports `Indeterminate`. Custom nameservers, search
domains, and retry policy are not yet public.

## Buffered HTTP/1.1

`HTTP.get` and the other `HTTP` module helpers create stateless requests. Use an
explicit `Client` to own connection reuse, statistics, and close behavior:

```kex
using Net.HTTP

let client = Client.open().try
let response = client.get("https://example.com/").try
if response.status.success? then
  IO.printLine(response.body.to(String).try)
else
  IO.printLine("HTTP ${response.status.code}")
end
IO.printLine("reused=${client.statistics.reusedConnections}")
client.close.try
```

The client validates request methods and headers, verifies HTTPS by default,
does not follow redirects, performs no hidden generic retry, bounds buffered
bodies, validates HTTP framing, and reuses only clean HTTP/1.1 connections.
Streaming bodies, cookies, redirects, compression, proxying, and concurrent
pool wait queues are not part of the current API.

The server supports immutable ordered routes, exact paths, named `:segments`,
terminal `*wildcards`, GET-to-HEAD fallback, generated OPTIONS/405 responses,
sequential persistent requests, bounded concurrent handlers, and graceful
shutdown:

```kex
using Net.HTTP
using Net.Socket

let health(request: Request<Binary>, context: Context) -> Response<Binary> =
  Response.text(200, "ok")

let router = Router.build.get("/health", ~health)
let server = Server.start(
  TCP.Endpoint.loopback(Net.Port.from(0).try),
  router
).try
IO.printLine("listening on ${server.localAddress.port.value}")
let report = server.stop.try
IO.printLine("completed=${report.completed} forced=${report.forced}")
```

Request lines, header sections/counts, bodies, handler concurrency, backlog,
and graceful shutdown are bounded. Streaming, middleware/groups, forms,
multipart, Expect/continue hooks, event feeds, and TLS serving remain tracked
work.

## WebSocket client

The BEAM client implements strict RFC 6455 `ws` and verified `wss` handshakes,
ordered subprotocols, masked client frames, high-level fragmented text/binary
messages, close validation, message limits, and automatic pong replies:

```kex
using Net.HTTP.WebSocket

let socket = WebSocket.connect("wss://example.com/events", ClientOptions {
  subprotocols: ["events.v1"]
}).try
socket.send(Text("hello"))
let message = socket.receiveMessage()
IO.printLine("${message}")
socket.close
```

`receiveMessage` is spelled out because `receive` is a Kex process keyword.
Extensions are not negotiated. Server upgrades, raw-frame access, heartbeat,
automatic reconnection, mocks, and browser WebSockets remain design work.

## Explicit retry

Networking never retries a request implicitly. Import `Control.Retry` and wrap
an operation when retrying is safe for that application:

```kex
using Control.Retry

let policy = Retry.exponential(
  4,
  100.milliseconds,
  2.seconds
).withMaximumElapsed(5.seconds).withJitter(0.25)
let response = Retry.run(policy, { |error| error.retryable? }) do
  client.get(url)
end.try
```

The helper supports fixed and capped exponential schedules, predicates,
elapsed-delay bounds, and symmetric bounded jitter. Production jitter uses a
cryptographically secure backend source. Tests can use `Retry.runWithRandom`
to inject both a sleeper and a `0.0..1.0` random sample without sleeping or
depending on nondeterminism. Retry cancellation and a full virtual-clock
capability remain design work.

# Kex Networking Plan

> **Status: implementation in progress on `net-redesign`.** The source-level
> APIs, rather than examples in this document, are authoritative. APIs marked
> **proposed** remain design targets and must not be treated as shipped.

Current checkpoint evidence:

| Checkpoint | Status | Implemented now | Still required by this plan |
|---|---|---|---|
| Foundations | Partial | Strict URI/URL values, explicit IRI conversion, query/form separation, IP/CIDR, ports, headers, statuses, typed errors, credential-safe URI/header inspection, fixed/exponential retry with predicates, elapsed-delay bounds, and an injected sleeper | Complete UTS #46 processing and retry jitter/injected randomness |
| Raw transports | Partial | Process-owned TCP and Unix streams with bounded chunk/exact/delimiter/line reads and half-close, UDP datagrams, address inspection, and direct TLS client primitives with typed unsupported tree fallbacks | Curated options/timeouts, bounded servers/events, multicast/broadcast, stale-path policy options, TLS upgrade/server/identity/session surface, scripted mocks |
| DNS | Partial | System resolver, A/AAAA/CNAME/MX/TXT/SRV/PTR values, bounded positive/negative cache and statistics | Typed custom nameservers/search/retry/timeout configuration, stronger DNSSEC reporting, scripted capability mock |
| HTTP client | Partial | Stateless helpers, explicit buffered client, strict response framing, bounded bodies, origin reuse, idle expiry/statistics/close | Concurrent bounded pool/wait queues, streaming/trailers API, redirects, cookies, compression, proxies/CONNECT, cancellation, replaceable transport mock |
| HTTP server | Partial | Ordered exact/named/wildcard routes, HEAD fallback, OPTIONS/405, buffered bodies, sequential keep-alive, bounded handlers, graceful stop | Body streaming/trailers, middleware/groups/error renderers, full context/events, deadlines, Expect/continue, forms/multipart, response validation, TLS serving |
| WebSocket | Partial | Verified `ws`/`wss` client handshake, subprotocols, masked sends, fragmented high-level receive, UTF-8/close/limit checks, automatic pong | Server upgrades, raw frames, heartbeat, reconnecting client, scripted mock, browser implementation |
| Browser wasm, examples, guides | Partial | Granular support values and typed unsupported paths for non-networking backends, source API examples, and the current networking guide | Fetch/WebSocket browser runtimes and Chromium CI, complete example inventory, and conformance vectors |

The executable coverage currently concentrates on foundational values, DNS
cache behavior, buffered HTTP client/server loopback behavior, and WebSocket
client protocol fixtures. The verification inventory near the end of this
document remains the completion gate.

## Syntax and API status

The examples use existing Kex language features: modules and `using`, records,
ADTs, generics, named arguments, blocks, capabilities, `Result`, `.try`,
`trying`, processes, and immutable receiver calls. Names under `URI`, `URL`,
`Net`, `Control.Retry`, and `Mock.Net` are proposed library APIs. Some examples
use proposed ownership-aware or typed-quantity signatures; those are labeled
and must not be mistaken for syntax Kex already accepts.

## Scope and delivery

Deliver the complete networking redesign as one ticket, implemented through
internal checkpoints:

1. Foundational URI, URL, headers, addressing, retry, and error types.
2. TCP, UDP, Unix sockets, server lifecycle, mocks, and typestates.
3. TLS and DNS.
4. HTTP/1.1 client, pooling, streaming, proxying, and cookies.
5. HTTP server, routing, middleware, and streaming.
6. WebSocket client/server.
7. Browser-wasm clients, specifications, examples, and documentation.

Nothing networking-specific enters the prelude. Remove `Http`, `Web.Server`,
`Mock.Http`, and their global types without aliases or breaking-change notices.
Do not modify issue #81 as part of this design work.

No public API may expose Erlang atoms, tuples, pids, option lists, exceptions,
or backend-specific structures.

## Modules and imports

- `using URI` provides opaque sibling `URI` and `URL` types and their
  modules/makes.
- `using Net.IP` provides addresses and CIDR networks.
- `using Net.DNS` provides names, resolvers, records, and lookup helpers.
- `using Net.Socket` provides `TCP`, `UDP`, `TLS`, and `Unix`.
- `using Net.HTTP` provides HTTP client, server, headers, bodies, and
  `Net.HTTP.WebSocket`.
- `using Control.Retry` provides generic retry policies.
- `using Mock` provides `Mock.Net.HTTP`, `.DNS`, `.Socket`, and `.WebSocket`.
- `Net.Support` replaces networking entries formerly exposed through prelude
  `Kex.Feature`.

Internal imports may satisfy public type dependencies, but importing one module
must not inject unrelated unqualified names.

## Foundational values

### URI and URL

- `URI` is an opaque, immutable RFC 3986 absolute-or-relative reference.
- `URL` is an opaque absolute hierarchical URI with authority; HTTP further
  requires `http` or `https`.
- `module URI`/`module URL` construct and parse values; `make URI`/`make URL`
  provide accessors, rendering, resolution, normalization, and immutable
  transformations.
- Strict parsing rejects malformed escapes, authorities, ports, and invalid
  text.
- Preserve original spelling for round-trip rendering and equality. Provide
  explicit `normalize` and `equivalent?`.
- Use UTS #46 non-transitional IDNA processing while preserving the display
  hostname and deriving its ASCII DNS form.
- `URI.fromIRI` explicitly converts Unicode IRIs; ordinary URI parsing remains
  ASCII-strict.
- Query parsing preserves ordering, duplicate keys, absent values, and empty
  values.
- Generic query encoding uses `%20`; form encoding lives separately under
  `URI.Form` and uses form rules.

```kex
# Proposed API; the Kex syntax is already implemented.
using URI

let raw = URI.parse("../users/Ada%20Lovelace?tag=kex&tag=net&empty=&flag#bio").try
let base = URL.parse("https://Example.COM:443/api/v1/").try
let resolved = base.resolve(raw).try

IO.printLine(resolved.string)             # preserves source spelling
IO.printLine(resolved.normalize.string)   # canonical explicit rendering
IO.printLine("${resolved.equivalent?(resolved.normalize)}")

let built = URL.build(
  scheme: "https",
  host: "api.example.com",
  path: ["v1", "people", "Ada Lovelace"],
  query: URI.Query.from([
    ("tag", Just("kex")),
    ("tag", Just("networking")),
    ("flag", None),
    ("empty", Just(""))
  ])
).try

let iri = URI.fromIRI("https://münich.example/straße").try
IO.printLine(iri.host.ascii)               # xn--mnich-kva.example
```

Form encoding is deliberately separate from generic query encoding:

```kex
# Proposed API.
let query = URI.Query.from([("q", Just("kex language"))])
IO.printLine(query.encode)                 # q=kex%20language

let form = URI.Form.from([
  ("name", "Ada Lovelace"),
  ("role", "author"),
  ("role", "admin")
])
IO.printLine(form.encode)                  # name=Ada+Lovelace&role=author&role=admin
let decoded = URI.Form.parse(form.encode).try
```

### Validated protocol values

Use opaque validated values with primitive overloads at high-level boundaries:

- `Net.Port`
- `Net.IP.Address` and `Net.IP.Network`
- `Net.DNS.Name`
- `Net.HTTP.Status`
- `Net.HTTP.Headers`

`Headers` preserves insertion order, duplicates, and original spelling while
providing case-insensitive lookup. Supply validated `empty`, `from`, `parse`,
`add`, `set`, `remove`, `get`, `getAll`, and `entries`.

```kex
# Proposed API.
using Net.HTTP

let headers = Headers.empty
  .add("Set-Cookie", "theme=dark")
  .add("set-cookie", "session=redacted")
  .set("Content-Type", "application/json")

IO.printLine("${headers.getAll("SET-COOKIE").length}")  # 2
```

Inspection and errors redact URI passwords and sensitive
authorization/cookie headers by default. Explicit accessors still return raw
values.

## Error behavior and security invariants

Expected parse, DNS, connection, TLS, protocol, timeout, cancellation, limit,
closed-handle, backend, and browser-policy failures return typed `Result`
errors. Receiving an HTTP 4xx or 5xx response is not a transport error. Errors
must retain structured context without exposing credentials or request bodies.
Backend-native error strings may be attached only as redacted diagnostic data,
not as the discriminant applications must match.

The design requires:

- bounded reads, buffers, queues, redirects, DNS indirections, multipart parts,
  decompression, and WebSocket messages;
- backpressure instead of unbounded buffering;
- hostname and certificate verification by default;
- explicit insecure TLS configuration;
- credential stripping across redirect origins and no HTTPS downgrade;
- strict HTTP framing validation to prevent request smuggling;
- no implicit removal of Unix socket paths;
- no silent browser option degradation;
- sensitive-header and URI-password redaction in inspection, errors, and events;
- cryptographically secure production randomness, with injectable deterministic
  time and randomness for tests.

Cancellation is explicit and idempotent. Timeouts identify the phase that
expired. Partial I/O errors report available progress. `close` is idempotent,
and operations on a closed resource return typed `Closed`.

## Shared runtime and server model

- All connection, listener, client, and server handles are opaque and
  process-safe.
- Hidden runtime owners serialize operations; handles may be passed between Kex
  processes.
- Concurrent sends are serialized. A second concurrent stream read returns
  typed `ReadInProgress`.
- Resources close explicitly or when their final Kex reference disappears.
- Use FileHandle-style typestates for stable capabilities—read/write permission,
  listener versus connection, plaintext versus TLS, and inbound versus outbound
  body direction—not for mutable open/closed lifecycle.
- Closing remains idempotent and every operation can still return `Closed`.

All long-running protocols use `Net.Server<E>`:

- `serve` blocks in the current process.
- `start` returns only after bind/listen initialization succeeds.
- The handle exposes `stop`, `join`, `running?`, `localAddress`, statistics, and
  a bounded event feed.
- Graceful stop rejects new work, waits for a configured grace period, then
  force-closes remaining handlers.
- Shutdown returns completed, failed, and forced-handler counts plus elapsed
  duration.
- Events cover readiness, connection acceptance/opening, handler failures,
  protocol failures, forced closes, overload, and shutdown.
- Optional callbacks receive the same typed events.
- Stream servers pause acceptance at their concurrency limit.

```kex
# Proposed API. Duration literals/typed quantities are proposed API surface.
let server = TCP.start(endpoint, options, handler).try
IO.printLine("listening on ${server.localAddress}")

let report = server.stop(grace: 10.seconds).try
IO.printLine("completed=${report.completed} forced=${report.forced}")
server.join.try
```

## Socket, TLS, IP, and DNS APIs

### IP and DNS

- `Net.IP.Address` supports IPv4/IPv6 parse, render, classification, and
  equality.
- `Net.IP.Network` supports CIDR parsing, containment, masks, and ranges.
- IPv6 zone/interface scope belongs to socket endpoint values, not the address
  itself.
- `Net.DNS` supports A, AAAA, CNAME, MX, TXT, SRV, and PTR plus high-level
  address resolution.
- Resolver values support system configuration or typed custom nameservers,
  retries, timeouts, and optional search domains.
- Use bounded positive/negative TTL caches with clear/statistics helpers.
- Expose DNSSEC response status but do not claim independent validation.
- Hostname and parsed-address socket overloads remain separate.
- High-level connects use Happy Eyeballs; this is address selection, not a
  general request retry.

```kex
# Proposed API.
using Net.IP
using Net.DNS

let address = Address.parse("2001:db8::42").try
let network = Network.parse("2001:db8::/32").try
IO.printLine("${network.contains(address)}")
IO.printLine("${address.private?} ${address.loopback?}")

let resolver = Resolver.system(cache: DNS.CacheOptions {
  entries: 1024,
  maximumTtl: 1.hours,
  negativeTtl: 30.seconds
})
let name = Name.parse("service.example").try
let addresses = resolver.addresses(name).try
let services = resolver.lookup(SRV, "_https._tcp.service.example").try
IO.printLine("cache hits: ${resolver.statistics.cacheHits}")
```

### TCP and Unix streams

Expose both primitives and helpers in the same protocol module:

- Connect, listen, accept, serve, send, receive-chunk, receive-exactly,
  receive-until, receive-line, half-close, address inspection, and close.
- Framed reads require explicit limits and return typed EOF/limit/timeout
  errors.
- Curated options cover no-delay, keepalive, send/receive buffers, backlog,
  reuse-address, and typed `Duration?` timeouts.
- `listener.serve(handler)` runs bounded concurrent handlers and isolates
  failures into server events.
- Unix sockets support filesystem-path streams only.
- Existing Unix paths are never deleted implicitly. A validated `removeStale`
  option is explicit; successfully closed listeners unlink paths they created.

```kex
# Proposed low-level TCP client.
using Net.Socket

let endpoint = TCP.Endpoint.host("example.com", Net.Port.from(80).try)
let connection = TCP.connect(endpoint, timeout: 10.seconds).try
connection.sendAll("ping\n".to(Binary).try).try
let reply = connection.receiveExactly(5.bytes, timeout: 30.seconds).try
connection.shutdownWrite.try
connection.close
```

```kex
# Proposed high-level concurrent TCP server.
let listener = TCP.listen(
  TCP.Endpoint.any(7000),
  TCP.ListenOptions { backlog: 128, reuseAddress: true }
).try

listener.serve(maxHandlers: 1024) do |connection|
  let line = connection.receiveLine(limit: 1.mebibytes).try
  connection.sendAll(line).try
end.try
```

```kex
# Proposed Unix-domain client/server.
let address = Unix.Address.path("/tmp/kex-agent.sock").try
let server = Unix.start(address,
  Unix.ListenOptions { removeStale: true }
) do |connection|
  connection.sendAll(connection.receiveChunk(64.kibibytes).try).try
end.try

Unix.connect(address).try do |connection|
  connection.sendAll("hello".to(Binary).try).try
end.try
server.stop(grace: 10.seconds).try
```

### UDP

- Bind, send-to, receive-from, multicast membership/interface/TTL/loopback,
  broadcast, inspection, and close.
- `serve` runs bounded concurrent handlers.
- A handler returns zero or more typed destination/data datagrams.
- Outbound sends are serialized.
- Saturation drops new datagrams, increments counters, and emits rate-limited
  overload events.

```kex
# Proposed low-level UDP operations.
let socket = UDP.bind(UDP.Endpoint.any(5353)).try
let packet = socket.receiveFrom(limit: 64.kibibytes).try
socket.sendTo(packet.source, packet.data).try

# Proposed high-level request/reply server.
let server = UDP.start(UDP.Endpoint.any(9000), workers: 64) do |datagram|
  [UDP.Datagram { destination: datagram.source, data: datagram.data }]
end.try
```

Multicast membership, interface, TTL, loopback, and broadcast are explicit
options; broadcast is never enabled as a side effect of a send.

### TLS and typestate-guided upgrades

- `Net.Socket.TLS.connect/listen` plus upgrade from a TCP connection.
- Successful upgrade returns `TLS.Connection`; stale plaintext aliases fail at
  runtime.
- Enable TLS 1.2 and 1.3 by default.
- Verify certificates and hostnames against system trust by default; disabling
  verification is explicit.
- Accept identities and trust anchors from filesystem paths or in-memory PEM
  `Binary`.
- Support client certificates, server mTLS modes, ALPN, handshake deadlines,
  bounded session resumption, and SNI identity selection.
- Expose typed negotiated version, cipher, peer chain, verification result, and
  ALPN.
- Never expose OTP SSL terms.

```kex
# Proposed direct verified TLS client.
let config = TLS.ClientConfig.system(
  serverName: DNS.Name.parse("example.com").try,
  alpn: ["http/1.1"]
)
let connection = TLS.connect("example.com", 443, config).try
IO.printLine("${connection.session.version} ${connection.session.alpn}")
connection.close
```

```kex
# Proposed typestate API. Generic phantom parameters use implemented syntax;
# transition/ownership guarantees are proposed and require runtime stale-alias
# checks because Kex does not yet have linear ownership.
type Plain
type Secure
type Connection<State>

let plain: TCP.Connection<Plain> = TCP.connect("mail.example", 587).try
plain.sendAll("STARTTLS\r\n".to(Binary).try).try
let secure: TLS.Connection<Secure> = TLS.upgrade(
  plain,
  TLS.ClientConfig.system(serverName: DNS.Name.parse("mail.example").try)
).try
secure.sendAll("EHLO client.example\r\n".to(Binary).try).try
```

```kex
# Proposed TLS server with mTLS and SNI identities.
let options = TLS.ServerOptions {
  defaultIdentity: TLS.Identity.files("cert.pem", "key.pem").try,
  identities: sniIdentities,
  clientAuthentication: TLS.RequireClient(trustAnchors),
  handshakeDeadline: 10.seconds
}
let server = TLS.start(TCP.Endpoint.any(8443), options, handler).try
```

## HTTP/1.1

### Shared model and bodies

Define generic envelopes:

```kex
# Proposed API declarations; generic record syntax is implemented.
record Request<B> do
  method : Method
  target : URI
  headers : Headers
  body : B
end

record Response<B> do
  status : Status
  headers : Headers
  body : B
end
```

Aliases distinguish buffered outbound requests, streamed outbound requests,
inbound server requests, buffered client responses, streamed client responses,
and outbound server responses.

Body APIs follow these rules:

- Convenience helpers return bounded buffered `Binary`.
- Explicit streaming APIs return `BodyReader`.
- Outgoing streams use producer callbacks receiving `BodyWriter`.
- Known lengths are verified exactly; unknown lengths use HTTP/1.1 chunked
  encoding.
- Readers expose validated trailers after EOF; writers may finish with
  trailers.
- Cancellation is explicit, idempotent, and propagated from cancelled Tasks.
- Unread server bodies are drained only within configured limits before
  keep-alive reuse; otherwise the connection closes.

### Client

- `Net.HTTP.Client` is a process-safe explicit value owning its pool, resolver,
  optional cookie jar, and options.
- Module verb helpers use a documented stateless default client and accept
  validated `URL` or convenient `String` overloads.
- `Net.HTTP.Transport` is the replaceable capability beneath clients.
- Provide `request` plus GET, POST, PUT, PATCH, DELETE, HEAD, and OPTIONS
  helpers.
- Pool connections per origin with total/per-origin limits, bounded wait queues,
  idle expiry, statistics, and graceful close reports.
- Redirects are disabled by default.
- Explicit redirect policy uses strict 301/302/303/307/308 semantics, strips
  credentials across origins, blocks HTTPS downgrade, detects loops, exposes
  history, and refuses streaming-body replay.
- High-level responses negotiate and decode supported compression with
  decompressed-size limits; low-level callers may request raw bytes.
- Cookie persistence requires an explicit typed `CookieJar`.
- Support explicit HTTP proxies and HTTPS CONNECT, proxy authentication, bypass
  lists, and opt-in environment configuration.
- Networking performs no generic hidden retry. `Control.Retry` composes around
  typed HTTP results.

```kex
# Proposed stateless buffered helper.
using Net.HTTP

let response = HTTP.get("https://api.example.com/status").try
if response.status.success? then
  IO.printLine(response.body.utf8.try)
else
  IO.printLine("HTTP ${response.status.code}")
end
```

```kex
# Proposed explicit pooled client with opt-in cookies, proxy, and redirects.
let client = Client.open(ClientOptions {
  resolver,
  pool: PoolOptions {
    perOrigin: 8,
    total: 128,
    queuedRequests: 256,
    idleExpiry: 30.seconds
  },
  cookies: Just(CookieJar.memory),
  proxy: Just(Proxy.connect("http://proxy.example:8080").try),
  redirects: RedirectPolicy.follow(maximum: 5)
}).try

let response = client.get(URL.parse("https://example.com/private").try).try
IO.printLine("redirects=${response.history.length}")
IO.printLine("pool=${client.statistics.openConnections}")
let closeReport = client.close(grace: 10.seconds).try
```

```kex
# Proposed streaming download. The response block owns the BodyReader.
client.stream(Request.get(downloadURL)) do |response|
  FS.File.open("archive.tar", FS.Write) do |file|
    response.body.eachChunk do |chunk|
      file.write(chunk).try
    end
    let trailers = response.body.trailers.try
  end.try
end.try
```

```kex
# Proposed streaming upload with verified known length.
let body = BodySource.produce(length: Just(size)) do |writer|
  source.eachChunk { |chunk| writer.write(chunk).try }
  writer.finish(trailers: checksumHeaders).try
end
client.post(uploadURL, body: body).try
```

### Server

- Move the server to `Net.HTTP.Server`.
- Support blocking `serve` and asynchronous `start`.
- Handlers receive `Request<BodyReader>` plus typed `Context` containing
  peer/local endpoints, TLS session, server identity, request ID, route
  parameters, and typed extension keys.
- Handlers return `Result<Response<BodySource>, HandlerError>`.
- Configure a default error renderer with route-group and route overrides.
- Middleware uses an outer-to-inner/inner-to-outer onion model and supports
  global and route-group chains.
- Routing supports exact paths, named segments, terminal wildcards, decoded
  captures, and declaration-order determinism.
- Split paths before percent-decoding captures so encoded slashes remain inside
  their segment.
- Trailing slashes are distinct unless a route group explicitly normalizes or
  redirects.
- Generate HEAD fallback, OPTIONS, 405, and `Allow` unless explicitly
  overridden.
- Validate outgoing responses; do not silently repair forbidden
  status/body/header combinations.
- Stream failures after headers abort the connection and publish typed events.
- Implement sequential persistent HTTP/1.1 requests per connection; do not
  promise pipelined parallel execution.
- Handle `Expect: 100-continue` automatically with a typed server pre-body
  accept/reject hook.
- Provide safe text, binary, form, and streaming multipart helpers.
- JSON and filesystem integration live in separate opt-in adapters.
- Form/multipart server parsing is explicit and bounded.

```kex
# Proposed routing, typed context, onion middleware, and error rendering.
let router = Router.new
  .use(requestId)
  .use(accessLog)
  .group("/api", middleware: [authenticate]) do |api|
    api.get("/users/:id") do |request, context|
      let id = context.route.parameter("id").try
      findUser(id).map { |user| Response.json(200, user) }
    end
    .onError(renderAPIError)
  end
  .get("/files/*path") do |request, context|
    # `/files/a%2Fb` captures `a/b` inside one segment.
    Response.text(200, context.route.parameter("path").try)
  end

let options = ServerOptions {
  headerDeadline: 10.seconds,
  handlerDeadline: 30.seconds,
  expectContinue: { |request, context| authorizeBody(request, context) }
}
let server = Server.start(TCP.Endpoint.any(8080), router, options).try
server.events.each { |event| recordServerEvent(event) }
```

```kex
# Proposed bounded forms and streaming multipart.
router.post("/profile") do |request, context|
  let form = request.body.form(limit: 64.kibibytes).try
  updateProfile(form.get("display_name").try)
    .map { |_| Response.redirect("/profile", status: 303) }
end

router.post("/upload") do |request, context|
  request.body.multipart(
    totalLimit: 20.mebibytes,
    partLimit: 10.mebibytes
  ).try.eachPart do |part|
    match part do
      FilePart(name, filename, headers, body) => storeUpload(name, filename, body).try
      FieldPart(name, value) => storeField(name, value).try
    end
  end
  Ok(Response.empty(204))
end
```

```kex
# Proposed blocking serve and graceful asynchronous shutdown.
Server.serve(TCP.Endpoint.loopback(8080), router).try

let running = Server.start(TCP.Endpoint.any(8081), router).try
let report = running.stop(grace: 10.seconds).try
IO.printLine("done=${report.completed} forced=${report.forced}")
running.join.try
```

## WebSocket

WebSocket is placed under `Net.HTTP.WebSocket`.

- Support client connections and authenticated server upgrade routes.
- A handshake callback returns `Accept(handler, headers:, subprotocol:)` or
  `Reject(Response)`.
- Clients offer ordered subprotocols; servers select at most one.
- Do not implement extensions or permessage-deflate.
- High-level receive returns reassembled text, binary, and close messages.
- Low-level receive exposes RFC 6455 frames and control frames.
- Validate UTF-8, masking, fragmentation, close codes, payload limits, and
  handshake fields.
- Automatically answer ping frames while still supporting explicit raw-frame
  access.
- Heartbeats are opt-in with ping intervals and pong deadlines.
- Basic connections never reconnect automatically; a separate reconnecting
  client supports bounded backoff and reports connection generations.
- Provide scripted mock connections with queued incoming messages and captured
  outgoing messages.

```kex
# Proposed client with ordered subprotocols and opt-in heartbeat.
using Net.HTTP

let socket = WebSocket.connect(
  URL.parse("wss://chat.example/socket").try,
  WebSocket.ClientOptions {
    subprotocols: ["chat.v2", "chat.v1"],
    heartbeat: Just(WebSocket.Heartbeat {
      interval: 30.seconds,
      pongDeadline: 10.seconds
    })
  }
).try

socket.send(Text("hello")).try
match socket.receive.try do
  Text(value) => IO.printLine(value)
  Binary(value) => consumeBinary(value)
  Close(code, reason) => IO.printLine("closed ${code}: ${reason}")
end
```

```kex
# Proposed authenticated upgrade route.
router.get("/socket") do |request, context|
  authenticate(request).map do |principal|
    WebSocket.upgrade(request) do |handshake|
      if handshake.subprotocols.contains?("chat.v2") then
        Accept(
          { |socket| serveChat(principal, socket) },
          headers: Headers.empty,
          subprotocol: Just("chat.v2")
        )
      else
        Reject(Response.text(426, "chat.v2 required"))
      end
    end
  end
end
```

```kex
# Proposed raw-frame and reconnecting clients.
let frame = socket.receiveFrame.try
match frame do
  Ping(payload) => observePing(payload)  # runtime also answers automatically
  Continuation(final?, payload) => inspectFragment(final?, payload)
  _ => Void
end

let reconnecting = WebSocket.Reconnecting.connect(
  url,
  policy: Control.Retry.exponential(maximumAttempts: 8)
).try
reconnecting.events.each do |event|
  match event do
    Connected(generation, socket) => resumeFrom(generation, socket)
    Disconnected(generation, error) => recordDisconnect(generation, error)
    Stopped => Void
  end
end
```

## Generic retry support

- Add `Task.sleep(Duration)` as a general prelude utility.
- Add opt-in `Control.Retry`.
- `Retry.run` operates on `Block<Result<X,E>>`.
- Support fixed and exponential policies, maximum attempts, maximum elapsed
  duration, typed predicates, and bounded jitter.
- Time and randomness are injectable capabilities so specs do not sleep or
  depend on nondeterministic values.

```kex
# Proposed API. `trying`, `.try`, blocks, and capabilities are implemented.
using Control.Retry

let policy = Retry.exponential(
  initial: 100.milliseconds,
  maximum: 5.seconds,
  maximumAttempts: 5,
  maximumElapsed: 20.seconds,
  jitter: Retry.Bounded(0.25),
  when: { |error| retryable?(error) }
)

let response = Retry.run(policy) do
  client.get(url)
end.try
```

Specs inject a virtual clock and deterministic random source:

```kex
# Proposed capability-based deterministic retry spec.
let result = Retry.run(policy, clock: fakeClock, random: fixedRandom) do
  scriptedOperation.next
end
assert(fakeClock.sleeps == [100.milliseconds, 200.milliseconds])
```

## Capability-based mocks

`Net.HTTP.Transport` and corresponding DNS/socket/WebSocket capabilities allow
scoped substitution without process-global interception. Scripted mocks consume
queued outcomes deterministically and capture calls. Empty scripts return typed
mock errors. They open no real sockets and work on every backend.

```kex
# Proposed HTTP transport mock.
using Mock

let transport = Mock.Net.HTTP.transport([
  Expect.get("https://example.test/users/42")
    .respond(Response.text(200, "Ada")),
  Expect.get("https://example.test/users/43")
    .fail(HTTPError.Timeout)
])
let client = Net.HTTP.Client.open(transport: transport).try

assert(client.get("https://example.test/users/42").try.body.utf8.try == "Ada")
transport.verify.try
```

```kex
# Proposed DNS and WebSocket mocks.
let resolver = Mock.Net.DNS.resolver([
  Answer.addresses("service.test", [Net.IP.Address.parse("127.0.0.1").try])
])

let socket = Mock.Net.WebSocket.connection(
  incoming: [Text("one"), Ping(Binary.empty), Close(1000, "done")]
)
socket.send(Text("outgoing")).try
assert(socket.outgoing == [Text("outgoing")])
```

Socket mocks additionally script partial reads/writes, EOF, timeouts, and close;
server mocks expose the same bounded events and shutdown reports as real
servers.

## Backend support

### BEAM

Implement the full networking surface with OTP internals hidden behind typed
Kex runtimes. Target current supported macOS, glibc Linux, and musl Linux.
Windows remains unsupported.

### Interpreter

- URI, URL, IP, headers, retry policy logic, routing, and other pure operations
  work normally.
- Real network operations return typed `UnsupportedBackend`.
- HTTP, DNS, sockets, and WebSocket scripted capability mocks work.

### Browser wasm

- Implement HTTP clients through Fetch and WebSocket clients through the
  browser API.
- Support streamed downloads and streamed uploads where the browser allows
  them.
- Raw sockets, DNS, TLS controls, servers, Unix sockets, and server WebSockets
  return typed unsupported errors.
- Unsupported Fetch options fail as `UnsupportedOption`; never ignore them.
- Ambient browser credentials are omitted by default and require an explicit
  browser credential option.
- CORS, mixed-content, and forbidden-header failures map to
  `BrowserRestricted`.
- A hidden cross-origin manual redirect maps to `OpaqueRedirect`; it is not
  followed.
- Browser WebSockets expose text/binary/open/close/subprotocol behavior. Raw
  frames, ping/pong control, custom handshake headers, and TLS controls return
  typed unsupported errors.
- `Net.Support` reports both compiled and currently usable granular
  capabilities.
- Required CI uses headless Chromium with local HTTP/WebSocket fixtures; Node
  remains for non-browser wasm tests.

```kex
# Proposed browser-portable subset.
let support = Net.Support.current
if support.httpClient.usable? then
  Net.HTTP.get("/api/status", browserCredentials: Net.HTTP.Omit).try
else
  Error(UnsupportedBackend(Net.HTTPClient))
end

let socket = Net.HTTP.WebSocket.connect("wss://example.test/events").try
socket.send(Text("hello")).try
```

Attempts to set a forbidden browser header, use manual cross-origin redirect
details, inspect raw WebSocket frames, configure TLS, or start a listener return
the typed errors above. The browser implementation never claims those options
were applied.

### Support matrix

| Surface | BEAM | Interpreter | Browser wasm |
|---|---:|---:|---:|
| URI/URL/IRI/query/form | Full | Full | Full |
| IP and header pure operations | Full | Full | Full |
| Retry policy and routing logic | Full | Full | Full |
| DNS | Full | Mock only | Unsupported |
| TCP and UDP | Full | Mock only | Unsupported |
| Unix filesystem streams | Full | Mock only | Unsupported |
| TLS and TLS controls | Full | Mock only | Unsupported |
| HTTP client | Full | Mock transport only | Fetch subset |
| HTTP server | Full | Mock only | Unsupported |
| WebSocket client | Full | Mock only | Browser message subset |
| WebSocket server/raw frames | Full | Mock only | Unsupported |
| Scripted capability mocks | Full | Full | Full |
| Granular `Net.Support` | Full | Full | Full |

## Default resource profile

Centralize and document overridable conservative defaults:

| Resource | Default |
|---|---:|
| HTTP request line | 8 KiB |
| Header section | 64 KiB and 100 fields |
| Buffered HTTP body | 10 MiB |
| Stream chunk | 64 KiB |
| WebSocket message | 16 MiB |
| TCP framed read | 1 MiB unless overridden |
| Server backlog | 128 |
| Concurrent stream handlers | 1,024 |
| UDP workers | 64 |
| Header deadline | 10 seconds |
| Body-idle and connection-idle deadline | 30 seconds |
| Handler deadline | 30 seconds, overridable per route |
| Graceful shutdown | 10 seconds |
| Server event buffer | 1,024 |
| HTTP pool | 8 per origin, 128 total, 256 queued requests |
| Client deadlines | 10-second connect; 30-second request |
| Idle pool expiry | 30 seconds |
| DNS cache | 1,024 entries; TTL cap 1 hour; negative TTL 30 seconds |

Use typed data quantities and `Duration`, with server-, route-, client-, and
operation-level overrides.

## Specification matrix

The implementation must pin the exact editions used and keep compact vectors
with provenance.

| Area | Governing specifications and policy |
|---|---|
| URI resolution/normalization | RFC 3986 |
| IRI conversion | RFC 3987 plus UTS #46 non-transitional IDNA |
| Query and form encoding | RFC 3986 and HTML/WHATWG form rules, kept separate |
| IPv4/IPv6 and CIDR | RFC 4291, RFC 4632, RFC 5952 |
| DNS | RFC 1034/1035 and record-specific RFCs; report, do not validate, DNSSEC |
| Happy Eyeballs | RFC 8305 behavior |
| TLS | TLS 1.2/1.3, RFC 6125 identity verification, current platform policy |
| HTTP semantics | RFC 9110 |
| HTTP/1.1 framing | RFC 9112 |
| Multipart forms | RFC 7578 |
| Cookies | Current selected RFC 6265bis revision |
| WebSocket | RFC 6455; no extensions/permessage-deflate |
| Browser subset | Fetch, URL, and WebSocket standards as exposed by browsers |

## Specifications and verification inventory

Create focused executable specs for:

- URI, URL, IRI/IDNA, query, and form encoding.
- Headers, Status, Port, IP Address/Network, and DNS.
- TCP, UDP, Unix, TLS, server handles, typestates, ownership, overload, and
  shutdown.
- HTTP bodies, trailers, pooling, proxying, cookies, redirects, compression,
  cancellation, and mocks.
- HTTP routing, middleware, context keys, forms, multipart, keep-alive, limits,
  and errors.
- WebSocket handshake, subprotocols, messages, raw frames, heartbeat, reconnect,
  and mocks.
- Retry schedules, predicates, jitter, cancellation, and deterministic time.
- Browser Fetch/WebSocket behavior and every unsupported-operation path.

Add cross-module loopback integration suites for HTTP/HTTPS, TCP, UDP,
TLS/mTLS/SNI, Unix sockets, WebSocket/WSS, DNS, proxy CONNECT, streaming,
cancellation, and graceful shutdown.

Vendor compact conformance vectors with provenance for URI/IDNA, HTTP parsing,
WebSocket framing, IP/DNS, and TLS policy. Required tests remain offline and
deterministic. Optional local interoperability suites may use OTP, OpenSSL, and
curl.

## Example inventory

Ship progressive example pairs—small helper-first examples plus realistic
low-level applications—for:

| Area | Required examples |
|---|---|
| URI/URL | construction, parsing, normalization, resolution, query, form, IRI/IDNA |
| DNS/IP | lookup, custom resolver/cache, address and network operations |
| TCP | low-level echo client; concurrent bounded service |
| UDP | request/reply, forwarding, multicast, broadcast |
| TLS | echo, verified client, upgrade, mTLS, SNI |
| Unix | filesystem-domain IPC and stale-path policy |
| HTTP client | buffered/streaming, pools, cookies, proxy, explicit redirects |
| HTTP server | routing, middleware, forms, multipart, streaming, TLS, shutdown |
| WebSocket | echo, authenticated chat, raw frames, heartbeat, reconnect |
| Browser wasm | Fetch and message-level WebSocket clients, unsupported paths |
| Testing | HTTP/DNS/socket/WebSocket capability mocks and virtual time |
| Retry | fixed/exponential policies, predicates, jitter, cancellation |

Update the language specification and dedicated networking guides during
implementation, and ensure every public function/type includes concise examples
and documented failure behavior. Those implementation and documentation edits
are outside this design-only change.

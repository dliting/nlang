# 23. The http Package

The `http` package speaks HTTP/1.1 (RFC 9112): a server and a
client, built on the `net` package's Transmission Control Protocol
(TCP) classes (chapter 21). It owns the wire format — request and
response parsing, the body framings, the hardening rules — and
nothing above it: routes, middleware, sessions and templates belong
to the web-framework layer that sits on top of this package. The
package is pure NLang source (`stdlib/http.n`) with no `native`
member of its own; the standard-library packages it uses are `net`,
`url` (chapter 22) and `gzip` (chapter 24).

Four things are deliberately outside its scope, each for a stated
reason. Transport layer security: the `net` package has none, so an
`https` URL is refused before a socket is opened, with a message
naming the cause. Request pipelining: a connection carries one
request at a time, as current HTTP/1.1 clients do — concurrency
comes from reusing several connections rather than from interleaving
requests on one. The WebSocket upgrade: an `Upgrade` header reaches
the handler untouched, because the package does not take the
connection over itself. Bidirectional reuse of one connection: the
client sends the whole request body before it starts reading the
response.

The package declares:

- `HttpRequest` and `HttpResponse`, the message objects (§23.2);
- `HttpHeaders`, a case-insensitive multi-value header map (§23.3);
- `HttpBodyReader`, the framing-aware body source behind a streaming
  body (§23.4);
- `HttpServer`, an accept thread plus one thread per connection
  (§23.5);
- `HttpClient`, a per-origin connection pool with redirects and
  timeouts (§23.6);
- `HttpException`, the failure class, which carries an HTTP status
  when the failure has one (§23.8, §8.3);
- the free functions `http.get`, `http.post`, `http.put`, `http.del`,
  `http.request` and `http.defaultClient` (§23.7).

A user package named `http` collides with the standard library and
is refused by the duplicate-package rule (§11.1).

## 23.1 A server and a client in one process

```nlang
import io;
import http;

HttpResponse handle(HttpRequest req) {
    HttpResponse res = HttpResponse();
    if (req.method == "POST") {
        res.body = io.toBytes("echo: " + req.bodyText());
        return res;
    }
    res.body = io.toBytes("hello, " + req.url.path.substring(1));
    return res;
}

int main() {
    HttpServer srv = HttpServer();
    srv.start("127.0.0.1", 0, handle);
    string base = "http://127.0.0.1:" + srv.localPort().toString();

    HttpResponse res = http.get(base + "/world");
    io.print(res.statusCode.toString() + " " + res.bodyText());
    HttpResponse echoed = http.post(base + "/echo", "round trip");
    io.print(echoed.bodyText());
    srv.close();
    if (res.statusCode == 200 && echoed.bodyText() == "echo: round trip") {
        return 23;
    }
    return 1;
}
```

The program prints `200 hello, world` and `echo: round trip`, and
exits 23 only when both answers are right (the self-check idiom of
§9). It shows the whole shape of a working exchange:

- A handler is a function that takes an `HttpRequest` and returns an
  `HttpResponse` — the type `func<HttpResponse, HttpRequest>`. A
  top-level function like `handle` above, a bound method, or a
  variable of that type are all handlers.
- `start(host, port, handler)` binds the listening socket, starts the
  accept thread and returns, so the same program can be a client.
  Port `0` asks for any free port, and `localPort()` (with
  `localAddress()`) then reports what was chosen — which is how this
  example avoids claiming a fixed port.
- `http.get` and `http.post` are the free functions of §23.7, running
  on a shared client. A request body is a `byte[]`; `io.toBytes`
  encodes text as Unicode Transformation Format (UTF-8) (§14.5), and
  `bodyText()` decodes it back.
- `srv.close()` releases the listening socket. A server meant for
  other machines binds `0.0.0.0` instead of `127.0.0.1`; a standalone
  server process calls `serve(...)` instead of `start(...)`, because
  `main` returning ends the process (§10.9).

The shipped examples build on the same shape: `examples/http_hello.n`
keeps the connection alive across several requests,
`examples/http_conc_bench.n` drives a thousand concurrent workers
through one client, and `examples/http_sse.n` streams events and
stops the producer when the client hangs up.

## 23.2 HttpRequest and HttpResponse

Both message objects are plain data: fields you set before sending,
fields the library fills after receiving.

| `HttpRequest` field | Notes |
| --- | --- |
| `method` | `"GET"` at construction. The server fills it from the wire; only upper-case token characters are accepted there (§23.5). |
| `url` | A `Url` (chapter 22). Client side: must be absolute and `http`, or the call raises `HttpException` before connecting. Server side: the parsed request target — origin form, absolute form and `*` are all read. |
| `version` | `"HTTP/1.1"` at construction; the parser stores what the peer announced. |
| `headers` | An `HttpHeaders` map, created empty by the constructor (§23.3). |
| `body` | Client side: the buffered request body, sent with a `Content-Length` the library computes. Exclusive with `bodyChannel`. Server side: always `null` — a handler reads the request body through `bodyReader`. |
| `bodyChannel` | Client side: a `Channel<byte[]>` to upload as a chunked stream (§23.4). Exclusive with `body`. |
| `bodyReader` | Server side: the request body as a framing-aware reader (§23.4). Client side: set by `open()`, `null` after a buffered `request()`. |
| `bodyText()` | The body decoded as UTF-8, working whichever of `body`, `bodyReader` carries it; `""` when there is no body. |
| `connectionClose` | The parsed `Connection: close` (request) or the answer you want (response): set it on a response to close the connection after it. |
| `framingMode`, `framingLength` | What the parser concluded about the body framing — `HttpParser.MODE_NONE`, `MODE_LENGTH`, `MODE_CHUNKED` or `MODE_EOF`, with `Content-Length` when the mode is `MODE_LENGTH`. A handler that wants to reason about framing reads these; one that reads `bodyReader` does not need them. |

| `HttpResponse` field | Notes |
| --- | --- |
| `statusCode`, `reasonPhrase` | `200` and `"OK"` at construction. The library names the phrase for the statuses it produces itself (§23.5); a status you invent keeps `"OK"` until you set the phrase, which is fine — the phrase is informational, and a client reads `statusCode`. |
| `version` | `"HTTP/1.1"` at construction; the parser stores what the peer announced. |
| `headers` | An `HttpHeaders` map (§23.3). |
| `body` | The buffered body, sent with a `Content-Length` the library computes. `null` means no body. A status below 200, a 204 or a 304 goes out without a body whatever this field holds, and a `HEAD` request is answered with the length but not the bytes. |
| `bodyChannel` | A `Channel<byte[]>` whose chunks go out as a chunked stream, with backpressure (§23.4). |
| `bodyReader` | Client side only: the streaming body handed back by `open()`. |
| `requestUrl` | The URL the response finally answers — after redirects (§23.6), the last hop; on the server, the request target echoed back. |
| `compressBody` | Server side: when `true` and the client accepts gzip, the buffered body is compressed and `Content-Encoding: gzip` plus `Vary: Accept-Encoding` are set (§23.6). |
| `connectionClose` | `true` closes the connection after this response. |
| `bodyText()` | Same reader-or-buffer logic as the request; `""` for an empty body. |

`Content-Type` is never invented for you: an empty response has no
`Content-Type`, and the free functions of §23.7 set
`text/plain; charset=utf-8` on the bodies they build.

## 23.3 HttpHeaders

| Member | Notes |
| --- | --- |
| `get(name)` | The first value of `name`, or `""` when absent. Names are matched case-insensitively, so `get("content-type")` finds `Content-Type`. |
| `getAll(name)` | Every value of `name`, in the order received; an empty `List` when absent. Use it for `Set-Cookie` and for any header the peer may repeat. |
| `set(name, value)` | Replaces every value of `name` with this one. |
| `add(name, value)` | Appends a second value, keeping the existing ones. |
| `remove(name)` | Drops every value of `name`. |
| `has(name)` | Whether the name is present, whatever its value. |
| `names()` | The distinct names, each spelled as it was first written. |
| `count()` | The number of name/value entries. |
| `copy()` | An independent map — the client uses one per attempt so a redirect cannot leave a rewritten header behind. |

`set` and `add` raise `HttpException` when a name or value contains
a bare carriage return or line feed, because either would split the
head on the wire. A peer that folds a multi-value header onto one
line (`Accept: gzip, deflate`) is yours to split: the parser hands
over the value as sent, and `HttpParser.splitTokens(value)` breaks it
on commas.

## 23.4 Bodies: framed for you

A message body travels in one of four framings: a `Content-Length`
count, a chunked stream, bytes until the connection closes, or no
body at all. The library picks the framing on send and recognises it
on receive; `HttpBodyReader` presents all four as one byte source, so
handler and client code never branch on the framing.

| `HttpBodyReader` member | Notes |
| --- | --- |
| `read(buf, timeoutMs = 0)` | Copies the next body bytes into `buf` and returns how many; `-1` once the body has ended. A short count is normal — one call may return fewer bytes than asked for. `timeoutMs <= 0` falls back to the client's `readTimeoutMs` or the server's `bodyTimeoutMs`; a deadline that passes raises `TimeoutException` (§20). |
| `readAll()` | The whole body as one `byte[]`. Over the size bound (`HttpServer.maxBodyBytes`, §23.5) it raises `HttpException` carrying status 413. |
| `bodyText()` | `readAll()` decoded as UTF-8. |
| `close()` | Stop early. An unexhausted body cannot be left behind on a pooled connection — the unread bytes would be mistaken for the next response — so the connection is closed instead of reused. |
| `drain(maxBytes)` | Consume the rest so the connection can be reused; `false` when the remainder exceeded `maxBytes`, which is the signal to close. The server runs this after each handler call. |

**Streaming responses.** A handler sets `bodyChannel` to a
`Channel<byte[]>` (§10.4) instead of `body`, and the body goes out as
a chunked stream. `send` blocks while the client is slow to read,
which is the backpressure: the producer runs no further ahead of the
socket than the channel's capacity allows. Closing the channel writes
the terminating chunk, and a `HEAD` request closes it instead of
pumping it, so a producer blocked in `send` still gets to finish. A
streaming answer to an `HTTP/1.0` client degrades to raw bytes framed
by the connection close, because chunked is an HTTP/1.1 framing
(RFC 9112 §9).

**Cancellation travels back.** If the client disconnects mid-stream,
the server's next write fails, the library closes the channel, and
the producer's next `send` raises `ChannelClosedException` — the
handler's own cleanup point. Nothing polls for a dead peer.

```nlang
import io;
import http;
import thread;
import concurrent;

class Feed {
    public static AtomicInt events = AtomicInt(0);
    public static AtomicInt stopped = AtomicInt(0);

    private Channel<byte[]> m_ch;
    public Feed(Channel<byte[]> ch) { m_ch = ch; }

    public void run() {
        try {
            for (int i = 0; i < 1000; i = i + 1) {
                m_ch.send(io.toBytes("data: tick " + i.toString() + "\n\n"));
                thread.sleep(2);
            }
        } catch (ChannelClosedException e) {
            Feed.stopped.incrementAndGet();
            return;
        }
        m_ch.close();
    }
}

HttpResponse feed(HttpRequest req) {
    HttpResponse res = HttpResponse();
    res.headers.set("Content-Type", "text/event-stream");
    res.bodyChannel = Channel(4);
    Feed f = Feed(res.bodyChannel);
    thread.spawn(f.run);
    return res;
}

int main() {
    HttpServer srv = HttpServer();
    srv.start("127.0.0.1", 0, feed);
    HttpClient cli = HttpClient();
    HttpRequest req = HttpRequest();
    req.url = Url.parse("http://127.0.0.1:" + srv.localPort().toString() + "/feed");
    HttpResponse res = cli.open(req);

    byte[] buf = byte[4096];
    string acc = "";
    while (Feed.events.load() < 3) {
        int n = res.bodyReader.read(buf, 5000);
        if (n < 0) { break; }
        acc = acc + io.fromBytes(buf[0:n]);
        while (acc.indexOf("\n\n") >= 0) {
            string ev = acc.substring(0, acc.indexOf("\n\n"));
            acc = acc.substring(acc.indexOf("\n\n") + 2);
            if (ev.startsWith("data: ")) { Feed.events.incrementAndGet(); }
        }
    }
    res.bodyReader.close();      // early disconnect: the connection is not pooled

    int waited = 0;
    while (Feed.stopped.load() == 0 && waited < 5000) {
        thread.sleep(10);
        waited = waited + 10;
    }
    cli.close();
    srv.close();
    if (Feed.events.load() >= 3 && Feed.stopped.load() == 1) { return 23; }
    return 1;
}
```

This is a server-sent events endpoint end to end: the handler returns
immediately while a producer thread feeds the channel, the client
takes the events one `read` at a time, and hanging up early stops the
producer. The event framing — `data:` lines separated by a blank
line — is written out by hand here on purpose: this package knows
chunked streams, not event-stream semantics, which belong to the
framework layer. The `read` loop is the honest shape of a streaming
body: bytes arrive in pieces, so a message boundary is searched for
in a growing accumulator rather than assumed per call.

**Streaming uploads.** Setting `req.bodyChannel` on a request sends
it chunked, with the same blocking behaviour on the producer side. A
streaming upload is consumed as it goes, so a redirect that keeps the
method (307 or 308) cannot replay it: that call raises `HttpException`
instead of silently sending an empty body (§23.6).

## 23.5 HttpServer

| Field | Default | Meaning |
| --- | --- | --- |
| `maxHeaderBytes` | 65536 | Header block larger than this: 431. |
| `maxHeaders` | 100 | More header fields than this: 431. |
| `maxBodyBytes` | 16777216 | Bound handed to the handler's body reader; `readAll` over it raises `HttpException` 413 (§23.4). |
| `keepAliveTimeoutMs` | 30000 | How long an idle connection may wait between requests. Also the total budget for a request head once its first byte has arrived (a slow client that dribbles bytes out gets 408). |
| `headerTimeoutMs` | 10000 | Per-wait budget inside that head phase. |
| `bodyTimeoutMs` | 10000 | Per-read budget for the handler's body reader. |
| `writeTimeoutMs` | 10000 | Per-write budget for the response. A large body is written in slices, so the bound is per slice, not per message. |
| `maxConnections` | 0 | `0` is unlimited. Over the cap, a new connection is answered 503 and closed. |
| `maxRequestsPerConnection` | 0 | `0` is unlimited. The Nth response also closes the connection. |

`start(host, port, handler)` returns once the socket is listening;
`serve(host, port, handler)` runs the same accept loop on the calling
thread and is the entry point for a standalone server, whose `main`
must not return (§10.9). `close()` stops accepting and wakes the
blocked `accept` call; connections already in flight finish the
requests of their own keep-alive loop.

The accept loop spawns one thread per connection (§10), and each
thread serves its connection's requests in order. A handler therefore
runs concurrently with other handlers and may block — on a slow
client, a `Channel`, or a `Mutex` — without stalling the server. There
is no thread pool to size: the connection count is the concurrency,
and `maxConnections` is the knob that caps it.

What the server does on its own, without consulting the handler:

| Status | Raised by |
| --- | --- |
| 400 | A malformed request line, method, request target or header line; a bare CR or bare LF where a carriage return and line feed (CRLF) pair belongs; a control character in the head; whitespace before a header colon; a name that is not a token; a missing or repeated `Host`; `Content-Length` together with `Transfer-Encoding`; a `Transfer-Encoding` other than a single `chunked`; a malformed or conflicting `Content-Length`; a malformed chunk size. |
| 408 | The head did not finish inside the header-phase budget. |
| 413 | The handler's `readAll` or `bodyText` passed `maxBodyBytes`. |
| 417 | An `Expect` header the server does not implement. A known `100-continue` is answered with a `100` interim response before the body is read, and the client skips interim responses. |
| 431 | The header block exceeded `maxHeaderBytes` or `maxHeaders`. |
| 500 | The handler raised an exception without a status, or returned `null`. The body says `internal error`: no NLang-internal detail reaches the wire. |
| 503 | The connection count reached `maxConnections`. |
| 505 | A request version other than `HTTP/1.1` or `HTTP/1.0`. |

Every one of these answers closes the connection, because a parse
failure leaves the stream position unknown. The handler's own
`HttpException` is the exception: its `status` field is used as the
answer when it names one (§23.8).

The head is parsed incrementally, so a peer that splits a request
across several segments or coalesces two requests into one segment is
handled without special casing. `HTTP/1.0` requests close after the
response unless they ask for `Connection: keep-alive`; `HTTP/1.1`
requests persist until `Connection: close` or an idle timeout. An
`Upgrade` header (including `websocket`) is left in `headers` for the
handler to act on, and the connection is not upgraded.

On the way out, the server supplies what a handler left to it: a
`Date` header when the response carries none, `Content-Length` for a
buffered body (`0` when there is none), `Transfer-Encoding: chunked`
for a streaming one, and the `Connection` header the answer implies —
including the `keep-alive` echo an `HTTP/1.0` request needs to
persist. Orderly close matters more than it looks: the server
half-closes and lets the peer finish, because a socket closed while
the peer still has bytes queued can arrive as a reset that discards
the answer the client had not read yet.

## 23.6 HttpClient

| Field | Default | Meaning |
| --- | --- | --- |
| `connectTimeoutMs` | 10000 | Deadline for establishing the connection. |
| `readTimeoutMs` | 10000 | Deadline for each read of the head and of a streaming body. |
| `callTimeoutMs` | 0 | Budget for the whole call, pool wait and redirects included; `0` waits as long as the per-operation timeouts allow. Exhausted: `TimeoutException`. |
| `maxRedirects` | 10 | `0` returns the `3xx` response itself instead of following it. |
| `maxConnsPerHost` | 0 | Cap on simultaneous connections to one origin; `0` is unlimited. A borrower that cannot get a connection within `callTimeoutMs` gets `TimeoutException`. |
| `defaultHeaders` | empty | Headers added to every request from this client — as gap fillers only: a header the request already sets wins. |
| `HttpClient.shared` | created on first use | The client the free functions use (§23.7). |

| Member | Notes |
| --- | --- |
| `request(req, callTimeoutMs = 0)` | Buffered exchange: the response body is read whole into `res.body`. A `callTimeoutMs` argument overrides the field for that call. |
| `open(req)` | Streaming exchange: `res.body` stays `null` and `res.bodyReader` is the body (§23.4). The caller owns the connection until the body is exhausted or `close()` is called. |
| `close()` | Closes every pool. Later calls on this client raise `HttpException` with the message `http: client pool closed`. |

`User-Agent` becomes `nlang-http/1.1` when neither the request nor
`defaultHeaders` sets it. `Host` is filled from the URL when the
request does not carry one.

**The pool.** Connections are pooled per origin (`host:port`). A
finished, healthy connection goes back for reuse; one that hit an
error, was told `Connection: close`, ended on a body read until
connection close, or was left with unread body bytes is closed
instead. At most four idle connections are kept per origin, and an
idle one older than ten seconds is dropped — shorter than the server's
own 30-second keep-alive timeout, so a client normally retires an
idle connection before a server closes it under it. A pooled
connection already closed by the peer is detected when borrowed and
replaced.

```nlang
import io;
import http;
import thread;
import concurrent;

class Tally {
    public static AtomicInt ok = AtomicInt(0);
    public static AtomicInt fail = AtomicInt(0);
}

class Worker {
    private HttpClient m_cli;
    private string m_base;
    private WaitGroup m_wg;

    public Worker(HttpClient cli, string base, WaitGroup wg) {
        m_cli = cli;
        m_base = base;
        m_wg = wg;
    }

    public void run() {
        try {
            HttpRequest req = HttpRequest();
            req.url = Url.parse(m_base + "/ping");
            HttpResponse res = m_cli.request(req);
            if (res.statusCode == 200 && res.bodyText() == "pong") {
                Tally.ok.incrementAndGet();
            } else {
                Tally.fail.incrementAndGet();
            }
        } catch (Exception e) {
            Tally.fail.incrementAndGet();
        }
        m_wg.done();
    }
}

HttpResponse ping(HttpRequest req) {
    HttpResponse res = HttpResponse();
    res.body = io.toBytes("pong");
    return res;
}

int main() {
    HttpServer srv = HttpServer();
    srv.start("127.0.0.1", 0, ping);
    string base = "http://127.0.0.1:" + srv.localPort().toString();

    HttpClient cli = HttpClient();
    cli.maxConnsPerHost = 4;          // 20 requests share 4 pooled connections
    WaitGroup wg = WaitGroup();
    wg.add(20);
    for (int i = 0; i < 20; i = i + 1) {
        Worker w = Worker(cli, base, wg);
        thread.spawn(w.run);
    }
    wg.wait();
    cli.close();
    srv.close();
    if (Tally.fail.load() == 0) { io.print("done " + Tally.ok.load().toString()); return 23; }
    return 1;
}
```

One client, twenty worker threads, four connections (§18.11): each
request waits its turn for a connection, and the answers come back
through the same few sockets. `examples/http_conc_bench.n` is this
program with a thousand workers.

**Redirects.** A `301`, `302` or `303` response with a `Location`
header is followed as a `GET` with the body dropped; `307` and `308`
keep the method and, when it was buffered, the body. `Location` is
resolved against the URL of the hop that sent it (chapter 22), so a
relative target works. `Authorization` and `Cookie` do not cross an
origin boundary: a redirect that changes scheme, host or port travels
without them. Each hop counts against `maxRedirects`; exceeding the
cap raises `HttpException` with the message `http: too many redirects`.
`res.requestUrl` names the URL the returned response answers.

**gzip.** On the buffered path the client adds
`Accept-Encoding: gzip` when the request sets none, the method is not
`HEAD` and there is no `Range` header — a HEAD response has no body to
inflate, and a `Range` response is a slice of the stored bytes, which
automatic decoding would misalign. When the answer comes back
`Content-Encoding: gzip`, the client inflates it into `res.body` and
removes both `Content-Encoding` and `Content-Length`. A response to a
request you set `Accept-Encoding` on yourself is handed over untouched:
asking for an encoding means reading it. `open()` neither adds the
header nor inflates, because `gzip.decompress` works on a whole buffer
(chapter 24); a gzipped streamed body must be decompressed by hand,
and the server side of the same feature is `HttpResponse.compressBody`
(§23.5).

## 23.7 The free functions

| Function | Notes |
| --- | --- |
| `http.get(url, timeoutMs = 0)` | `GET`, response body buffered. |
| `http.post(url, body, contentType = "text/plain; charset=utf-8", timeoutMs = 0)` | `POST` with `body` encoded as UTF-8. |
| `http.put(url, body, contentType = "text/plain; charset=utf-8", timeoutMs = 0)` | `PUT` with `body` encoded as UTF-8. |
| `http.del(url, timeoutMs = 0)` | `DELETE` — the free function is named `del` because `delete` is a keyword. |
| `http.request(req, timeoutMs = 0)` | Any request, buffered. |
| `http.defaultClient()` | The shared client the others use, created on first call. Configure it through this handle (`defaultHeaders`, the timeouts, `maxRedirects`, `maxConnsPerHost`), or assign your own client to `HttpClient.shared`. |

`url` is a `string`, parsed as in chapter 22. `timeoutMs > 0` bounds
that one call, overriding the shared client's `callTimeoutMs`. These
functions buffer: for a large body or a stream, build the request and
call `HttpClient.open()` (§23.6). Like every other client entry point,
they return the response for a `4xx` or `5xx` — only a transport or
protocol failure raises.

## 23.8 HttpException

`HttpException` derives from `IOException` (§8.3, §20) and is a
`const class`: constructed once, its fields do not change afterwards.
Its one field is `status`:

| `status` | Meaning |
| --- | --- |
| `0` | Not an HTTP status error — the failure happened before or beside a status line. |
| `4xx`, `5xx` | The status this failure answers with, or the status read from a peer's malformed message. |

A `cause` may ride along, as for any exception (§8.2). The messages
the library itself raises name the cause in plain words:
`http: request url must be absolute`, `http: unsupported scheme https
(no TLS in net.n)`, `http: header block exceeds 65536 bytes` (431),
`http: body exceeds 16777216 bytes` (413), `http: too many redirects`,
`http: cannot replay a streaming body across redirect`,
`http: client pool closed`, `http: malformed request line` (400). The
server's own answers are listed in §23.5.

A `4xx` or `5xx` from a peer is a response, not an exception: check
`statusCode`. Exceptions mean the exchange itself failed — a refused
connection, an expired deadline, a message that cannot be parsed.
That is what makes a handler's `HttpException` with a status a
request-level error the server can answer, and it is why the
`SocketException` and `TimeoutException` of chapter 20 stay
distinguishable underneath this package: they pass through unchanged.

# 27. The web Package

The `web` package is the application layer that sits on top of the
`http` package (chapter 23): an onion of middleware, one context per
request carrying the response helpers a handler needs, a route table,
sessions, mounted static-file serving, server-sent events (SSE) and
form parsing for both encodings a browser sends. It owns no sockets
of its own — `App` wraps an `HttpServer`, and the transport frame
(body caps, timeouts, connection pooling) stays exactly where chapter
23 documents it. The package is pure NLang source in four modules:
`stdlib/web.n` for the core, `stdlib/web/static.n` for static files,
`stdlib/web/sse.n` for event streams and `stdlib/web/form.n` for
forms. It declares no exception classes of its own: a refused request
arrives as the `HttpException` of chapter 23, a transport failure as
the `SocketException` of chapter 21, and a framework misuse as a
plain `Exception` (chapter 8).

The four modules spell one package. `import web;` binds the
package-level module under the name `web`; a type that lives in a
submodule needs that submodule imported too — `import web.static;`,
`import web.sse;` or `import web.form;` — or one `import web.*;`,
which merges the package module with all of its submodules and is
what the tour below uses. Once a module is in scope its members answer
in both spellings: `Form.parse(ctx)` and `web.form.Form.parse(ctx)`
are the same call, and a program may pick whichever reads better.
When a type spelled in one module appears in a name written in
another — a handler signature naming `web.form.FormData` — the module
qualification stays on the name. A user package named `web` collides
with the standard library and is refused by the duplicate-package rule
(§11.1).

Everything that shapes how the app answers closes the moment its
first request is handled: `use`, `useSessions` and the route
registrations all name that refusal in §27.2 and §27.5. The two hooks
that fill one slot each — `onError` and `setNotFound` — may be
replaced at any time, because a late write only means the requests
before it were answered in the other shape. The package also carries
a few framework-internal helpers (the hex spelling shared by session
ids and upload names, the value bag under sessions and attributes, the
function adapter behind `use`); they are visible in the source but are
not part of this chapter.

The package declares:

- `App`, the facade that owns the server (§27.2);
- `RequestContext`, the per-request state (§27.3);
- `Middleware`, the chain link (§27.4);
- `Router`, the route table (§27.5);
- `Session`, `SessionStore` and `MemorySessionStore` (§27.6);
- `web.static.StaticFiles`, the mounted file handler (§27.7);
- `web.sse.Sse`, the event-stream helper (§27.8);
- `web.form.Form`, `FormOptions`, `FormData`, `FormFile` and
  `MultipartBody`, the form face (§27.9).

## 27.1 A tour

The tour is the shipped example `examples/web_hello.n`; it starts a
server, walks every route with its own client and ends by itself, so
`main` can return and the process exits.

```nlang
// Example: a small web app that runs itself. Two middleware styles, a `:id`
// route, a session that survives a login rotation, an SSE feed, a mounted
// static handler, and a client that walks every route and checks the reply.
// `listen` starts the server on a throwaway port and returns, so main() keeps
// going and the process ends by itself — no serve() loop, no dangling thread.
// The manual's web chapter reproduces this file verbatim as its tour.
import fs;
import http;
import io;
import system;
import url;
import web.*;

// Function-style middleware: answers before the router ever sees the request.
void requireKey(web.RequestContext ctx) {
    if (ctx.req.headers.get("X-Key") != "demo") {
        ctx.text(401, "missing key");
        return;
    }
    ctx.next();
}

// Class-style middleware: wraps the rest of the chain.
class Timing : web.Middleware {
    public void process(web.RequestContext ctx) {
        long started = system.monoMs();
        ctx.next();
        ctx.res.headers.set("X-Elapsed-Ms", (system.monoMs() - started).toString());
    }
}

void showItem(web.RequestContext ctx) {
    ctx.text(200, "item " + ctx.param("id") + " query " + ctx.query);
}

void login(web.RequestContext ctx) {
    web.Session s = ctx.session();
    s.set("user", "dlite");
    s.rotate();                       // a privilege change needs a new id (D-12)
    ctx.text(200, "logged in");
}

void whoami(web.RequestContext ctx) {
    ctx.text(200, "user " + ctx.session().getString("user", "-"));
}

void feed(web.RequestContext ctx) {
    web.sse.Sse sse = web.sse.Sse(ctx.res);   // the helper sets the event-stream headers
    sse.send("one");
    sse.send("tick", "two");
    sse.close();
}

// One client-side builder for every leg: they differ only by method, path,
// key and session cookie.
http.HttpResponse ask(http.HttpServer srv, string method, string path,
                      string key, string cookie) {
    http.HttpRequest req = http.HttpRequest();
    req.method = method;
    req.url = Url.parse("http://127.0.0.1:" + srv.localPort().toString() + path);
    if (key != "") {
        req.headers.set("X-Key", key);
    }
    if (cookie != "") {
        req.headers.set("Cookie", cookie);
    }
    return http.request(req);
}

// The first name=value pair of a Set-Cookie header: attributes end at ';'.
string sessionCookie(http.HttpResponse res) {
    string line = res.headers.get("Set-Cookie");
    int semi = line.indexOf(";");
    if (semi < 0) {
        return line;
    }
    return line.substring(0, semi);
}

int main() {
    fs.makeDirs("public");
    io.writeFile(fs.join("public", "index.html"), "<h1>hello web</h1>");

    web.App app = web.App();
    app.use(Timing());
    app.use(requireKey);
    app.useSessions(web.MemorySessionStore());
    app.get("/items/:id", showItem);
    app.post("/login", login);
    app.get("/whoami", whoami);
    app.get("/feed", feed);
    web.static.StaticFiles assets = web.static.StaticFiles("public");
    app.mount("/static", assets.serve);
    app.listen("127.0.0.1", 0);                    // port 0 = an ephemeral one
    http.HttpServer srv = app.server();

    bool ok = true;

    // Middleware short-circuit: no key, so no route lookup happened at all.
    http.HttpResponse res = ask(srv, "GET", "/items/7?v=list", "", "");
    io.print("nokey=" + res.statusCode.toString());
    ok = ok && (res.statusCode == 401);

    // The `:id` route, the raw query string, and the wrapping middleware.
    res = ask(srv, "GET", "/items/7?v=list", "demo", "");
    io.print("item=" + res.bodyText());
    io.print("elapsed=" + res.headers.has("X-Elapsed-Ms").toString());
    ok = ok && (res.bodyText() == "item 7 query v=list");
    ok = ok && res.headers.has("X-Elapsed-Ms");

    // A login writes the session, so this reply carries the session cookie.
    res = ask(srv, "POST", "/login", "demo", "");
    string cookie = sessionCookie(res);
    io.print("login=" + res.statusCode.toString());
    io.print("issued=" + cookie.startsWith("nlang_session=").toString());
    ok = ok && (res.statusCode == 200);
    ok = ok && cookie.startsWith("nlang_session=");

    // The same request with that cookie reads the session back, and a reply
    // that only read the session sets no cookie again (D-12).
    res = ask(srv, "GET", "/whoami", "demo", cookie);
    io.print("whoami=" + res.bodyText());
    io.print("reissued=" + res.headers.has("Set-Cookie").toString());
    ok = ok && (res.bodyText() == "user dlite");
    ok = ok && (res.headers.has("Set-Cookie") == false);

    // An anonymous visit gets its own empty session, not somebody else's.
    res = ask(srv, "GET", "/whoami", "demo", "");
    io.print("anon=" + res.bodyText());
    ok = ok && (res.bodyText() == "user -");

    // Server-sent events, framed by the helper rather than by hand.
    res = ask(srv, "GET", "/feed", "demo", "");
    string frames = res.bodyText();
    io.print("feed=" + (frames.indexOf("data: one") == 0).toString()
             + " tail=" + frames.endsWith("\r\n\r\n").toString());
    ok = ok && (frames.indexOf("data: one") == 0);
    ok = ok && frames.endsWith("\r\n\r\n");

    // The mounted handler serves the directory index; anything else under that
    // prefix is a 404 written by the sub-tree's own serve() (D-10).
    res = ask(srv, "GET", "/static/", "demo", "");
    io.print("static=" + res.statusCode.toString() + " " + res.bodyText());
    ok = ok && (res.statusCode == 200);
    ok = ok && (res.bodyText() == "<h1>hello web</h1>");
    res = ask(srv, "GET", "/static/absent.css", "demo", "");
    io.print("missing=" + res.statusCode.toString());
    ok = ok && (res.statusCode == 404);

    app.drain(5000);                    // let in-flight requests finish (D-22)
    app.close();

    int rc = 0;
    if (ok == false) {
        io.print("mismatch");
        rc = 1;
    } else {
        io.print("ok");
    }
    return rc;
}
```

The program prints one line per leg and ends with `ok`. Read against
the transcript, the legs show the whole framework at work:

- `nokey=401` — `requireKey` answered the request itself and never
  called `ctx.next()`, so the route table was never consulted (§27.4).
- `item=item 7 query v=list` — the `:id` route captured the segment
  (percent-decoded by `param()`) and handed the handler the raw query
  string as it arrived; `elapsed=true` is the class middleware
  stamping its header after `next()` returned, around the route
  handler (§27.5).
- `login=200 issued=true` — the login wrote the session and rotated
  its id, so the reply carries the session cookie
  `nlang_session=<id>`; `whoami=user dlite` reads it back, and
  `reissued=false` records that a request which only read the session
  gets no new cookie (§27.6).
- `anon=user -` — an anonymous visit gets its own fresh empty
  session, not a stranger's.
- `feed=true tail=true` — the `Sse` helper framed two events on
  `Content-Type: text/event-stream`: the body opens with the
  `data: one` frame and every event ends with a blank line (§27.8).
- `static=200 <h1>hello web</h1>` and `missing=404` — the mounted
  `StaticFiles` handler answered its directory index, and a miss
  inside the mounted prefix is the subtree's own 404, not the app's
  (§27.7).
- `app.drain(5000)` waits out the requests already in flight,
  `app.close()` releases the listening socket, and `main` returns —
  the exit code is computed from `ok` (`0` on a clean run), which is
  the self-check shape the other examples use too (§9).

The comments carry letter-number tags (D-10, D-12, D-22) naming the
design decisions behind this package; each comment states the rule
its decision settled, so the tags only trace a rule back to it.

## 27.2 App

`App()` is the whole facade: it collects the middleware chain,
forwards the registrations to one route table, owns the `HttpServer`
and turns whatever the chain does or throws into a single response.

| Registration | Notes |
| --- | --- |
| `use(Middleware m)` | Appends a chain link (§27.4). |
| `use(func<void, RequestContext> fn)` | The same chain from a plain function, for a link too small for a class. |
| `useSessions(SessionStore store, bool secureCookies = false, bool hostPrefix = false)` | Turns sessions on (§27.6). The two switches are the session cookie's `Secure` attribute and the `__Host-` name prefix; asking for the prefix without `Secure` is refused at once, because a browser would silently drop the prefixed cookie rather than register it. |
| `on(string method, string pattern, func<void, RequestContext> h)` | Registers a route; `get`, `post`, `put`, `patch`, `head`, `del` and `any` are the verb shorthands (`del` because `delete` is a keyword, as in §23.7) (§27.5). |
| `mount(string prefix, func<void, RequestContext> h)` / `mount(string prefix, Router subtree)` | Hands the subtree under `prefix` to a handler or to another route table (§27.5). |
| `setNotFound(func<void, RequestContext> h)` | The app's own answer for a path no route serves. |
| `onError(func<void, RequestContext, Exception> h)` | Reshapes a failure after the plain 500 has been rebuilt (§27.2 below). |

| Runtime | Notes |
| --- | --- |
| `listen(string host, int port)` | Starts the server and returns: the accept loop runs on its own thread (§23.5), so `main` keeps going — the shape the tour uses. Port `0` asks for an ephemeral port, and `server().localPort()` reports the choice. |
| `serve(string host, int port)` | The same accept loop on the calling thread, the entry point for a standalone server process whose `main` must not return (§10.9). |
| `server()` | The owned `HttpServer` itself: the body caps and timeouts of §23.5 are set on this object, and `isServing()` is watched there — `App` deliberately forwards only `listen`, `serve`, `drain` and `close`, so one transport face serves both layers. |
| `drain(int graceMs = 15000)` | Forwards `HttpServer.drain` (§23.5): stop accepting, wait out in-flight requests, and answer with the number of connections the grace window forced closed. Repeatable, and `0` on a server that is not running. |
| `close()` | Forwards `HttpServer.close` (§23.5). After it the app can `listen` again — the route table, chain and sessions are untouched. |

Registration closes with the first request, because what a running
app looks like is a question with one answer: `use` then answers
`web.App.use: middlewares must be registered before the first
request.`, `useSessions` answers
`web.App.useSessions: sessions must be configured before the first
request.`, and a route registration arrives at the `web.Router`
message of §27.5. `useSessions` with `hostPrefix` and no
`secureCookies` answers
`web.App.useSessions: the __Host- cookie prefix requires the Secure
attribute.` even before that first request.

A failure inside the chain gets one treatment. An `HttpException`
(§23.8) answers with its own `status` and its own message as the body;
a status of `0` clamps to 500. Any other exception answers 500 with
the body `internal error` — no interpreter detail reaches the wire —
and then the `onError` hook, if set, may reshape the answer. The hook
starting from a plain answer is deliberate: a chain that threw has
already written part of a response, and the header table is replaced
wholesale so a `Content-Type` a middleware set before the throw
cannot ship beside an error body it never described. A hook that
itself throws stays a visible 500, mapped once more by the http layer
(§23.5).

Two startup failures answer from the layer underneath, and `App`
leaves their names alone because `listen` and `serve` share one start
path, so the same failure keeps the same message whichever door it
arrives through. Starting a server that is
already running answers
`http.HttpServer.start: already started by this instance.` (or
`http.HttpServer.serve: ...` through `serve`). A port another process
holds answers the `SocketException` of chapter 21,
`net: bind <host>:<port> failed: <system text>` — an ordinary
outcome a caller catches, and the failed start leaves the app
unstarted, so a corrected `listen` can try again.

## 27.3 RequestContext

One `RequestContext` serves one request: middleware and handlers
alike take it as their only argument, and the only thing that ever
leaves the app is the response it ends up carrying. A handler reads
the request through these fields:

| Field | Notes |
| --- | --- |
| `req`, `res` | The `HttpRequest` and `HttpResponse` of chapter 23, the pair the response helpers below write. |
| `method` | The request method as it arrived. |
| `path` | The raw, undecoded path; the router matches on these bytes, and decoding is the handler's own `param()` (§22, §27.5). |
| `query` | The raw, undecoded query string; `url.percentDecode` and the parsing faces of chapter 22 read it however the route wants. |
| `params` | The captured segments as the router left them; a handler reads them through `param()` below. |

| Route parameters | Notes |
| --- | --- |
| `param(string name)` | The captured segment, percent-decoded; a malformed escape returns the bytes as they came rather than failing the request, and a name nothing captured answers `""`. |
| `paramInt(string name, int def = 0)` | The same segment read as a number; a missing segment and a value that is no number both answer `def`. |
| `hasParam(string name)` | Whether the matched pattern captured this name. |

| Attributes | Notes |
| --- | --- |
| `set(string key, Object value)` / `has`, `remove` | The per-request key-value store, the place middleware leaves findings for later links (§27.4). |
| `get(string key, Object def = null)` | The stored value, or `def`. |
| `getString`, `getInt`, `getBool` | The typed reads, answering `""`/`0`/`false` by default. `getInt` and `getBool` read the stored value through its own text, so a number stored as text reads back as a number; a value that cannot read as the requested type is the caller's default, not an error. |

| Session | Notes |
| --- | --- |
| `session()` | This request's session (§27.6), loaded from the store on first use and cached afterwards, so every link in one chain shares one object. Reaching for it on an app that never called `useSessions` answers `web.App.useSessions: no session store configured.` |

| Response helpers | Notes |
| --- | --- |
| `text(int status, string body)` | `text/plain; charset=utf-8` (§14.5). |
| `html(int status, string body)` | `text/html; charset=utf-8`. |
| `json(int status, JsonValue value)` | The `serialize()` text of a chapter 26 value as `application/json`. |
| `bytes(int status, byte[] body, string contentType)` | Any body under any media type. |
| `redirect(string location, int status = 302)` | A `3xx` with the target in `Location` and no body. |
| `noContent()` | `204`, dropping any half-written body. |

Each helper sets the status, the matching reason phrase (§23.2) and
the body; a handler is free to keep editing `res` afterwards — the
helpers write the response, they do not send it.

| Cookies | Notes |
| --- | --- |
| `cookie(string name)` | The first `name=value` pair of that name in the request's `Cookie` header (§23.3); the value stays percent-encoded because nothing said it was encoded. |
| `setCookie(string name, string value, string path = "/", int maxAgeSeconds = 0, bool httpOnly = true, bool secure = false, string sameSite = "Lax")` | Appends one `Set-Cookie` line (each cookie needs its own line, so the header accumulates). `maxAgeSeconds` is tri-state: `0` leaves `Max-Age` out (a cookie that lives for the browser session), a positive number sends it, and a negative number sends `Max-Age: 0`, which drops the cookie on the client. |

`ctx.next()` advances the chain: it runs the next link, and returns
once the chain has answered (§27.4). Advancing twice from the same
link is a handler bug — a link that forwarded twice would run its
downstream twice with a response that only makes sense once — so it
answers the 500 `web: next() called more than once`.

## 27.4 Middleware

A middleware is one method:

```nlang
class Audit : web.Middleware {
    public void process(web.RequestContext ctx) {
        ctx.set("startedMs", system.monoMs());   // before: runs on the way in
        ctx.next();                              // the rest of the chain
        ctx.res.headers.set("X-Elapsed-Ms",
            (system.monoMs() - ctx.getInt("startedMs")).toString());
    }
}
```

The base class default for `process` is `ctx.next()`, so a middleware
that only wraps the rest of the chain writes its two halves around
`next()` and inherits everything else — the `Timing` class in the
tour is that shape. A link that never calls `next()` short-circuits:
it answers itself and neither the later links nor the route table
ever sees the request. A plain function needs no class: `use(fn)`
wraps a `func<void, RequestContext>` into the same chain, which is
what lets the tour register `requireKey` and `Timing()` side by side.

Registration order is onion order: the first `use` runs first and
returns last, and the route table rides the chain as its last link.
A shared middleware instance serves every request, so it must keep no
per-request state in its own fields — that is what the attribute store
of §27.3 is for.

## 27.5 Routing

A pattern is a `/`-separated path of three segment kinds: a literal,
`:name` capturing exactly one segment, and a trailing `*` eating
everything below it. Registration is sugar over one table:

```nlang
app.on("GET", "/items/:id", showItem);   // same as app.get(...)
app.get("/search", search);              // literal
app.mount("/static", assets.serve);      // a subtree hands off
app.get("/api/*", apiCatchAll);          // param("*") holds the rest
```

The table answers a request in two steps. A fully literal pattern
matches by hash lookup before the registration-order walk, so a
literal route keeps its own handler however many parameter routes were
registered ahead of it; between wildcard rows, the first registered
win. A `HEAD` request that finds no `HEAD` row rides the `GET` row.
The verbs a route answers come from the registration: `get` registers
`GET`, `any` registers all seven of `GET`, `HEAD`, `POST`, `PUT`,
`PATCH`, `DELETE` and `OPTIONS`, and a mount answers all seven
because the subtree below the prefix settles the method question
itself.

A miss is an answer, not an exception. If the path is registered
under other verbs, the app answers `405` with an `Allow` header
listing those verbs (with `HEAD` written beside the `GET` it rides)
and the body `method not allowed: <method>`; otherwise the
`setNotFound` hook answers if there is one, and the default is a plain
`404` with the body `not found: <path>`. Mounted subtrees report their
own misses the same way: `Router.serve` is the subtree's answer for a
request it does not serve, which is what makes a miss under
`/static/` say so with the subtree's own table (§27.7). The 405 also
skips `setNotFound` — a path the table knows under another verb is a
method mistake, not an unknown path.

`mount(prefix, subtree)` hands the subtree its requests with the
prefix stripped: under `mount("/static", ...)`, `/static/img/logo.png`
reaches the subtree as `/img/logo.png`, and a request equal to the
prefix reaches it as `/` (a root mount strips nothing). Once the
prefix matches, the subtree owns the answer whatever it then does
with it — its 404 is its own, and the outer table never sees the
request again. A request path is matched as it arrived: a trailing
slash is a segment of its own, so `/x/` and `/x` are different paths,
and only `param()` decodes.

Registration is where a mistake gets caught, and a refused
registration leaves the table exactly as it was:

| Refusal | Message shape |
| --- | --- |
| The same verb on the same path twice, or a row that an earlier one already answers | `web.Router: /a/:x conflicts with /a/b (registered first)` — the row that would never run is refused. |
| A malformed pattern | `web.Router: invalid pattern "<pattern>": <reason>` — the reasons name the mistake: the empty pattern, a missing leading `/`, a trailing `/`, an empty segment, a `*` anywhere but last, a `:` segment with no name, one segment name used twice, and a mount prefix that is not fully literal. |
| A registration after the first request | `web.Router: routes must be registered before the first request.` — the table a request has answered from stops growing (§27.2). |

## 27.6 Sessions

Sessions are opt-in: `useSessions` hands the app a store, and a
handler reaches this request's session through `ctx.session()`. The
server keeps the data; the client holds an id.

| `Session` member | Notes |
| --- | --- |
| `id` | 32 lower-case hex characters — 16 random bytes (`math.randomBytes`, §13.3) spelled as hex — which is what makes a guessed id worthless. |
| `isNew` | Whether this request was the one the session was minted for. |
| `isExpired` | Whether the id the client presented is dead: expired, forged or destroyed. A login page shows "your session timed out" from exactly this flag. |
| `expiresAtMs` | The expiry instant on the monotonic clock of chapter 15, refreshed by every `save`; `-1` once the session is destroyed. |
| `set`, `has`, `get`, `getString`, `getInt`, `remove` | The data, read with the same rules and the same defaults as the request attributes of §27.3. |
| `isDirty()` | Whether data was written this request; it is what decides whether the store sees a `save`. |
| `destroy()` | Forgets the session at once: the data goes, the server's record goes, and the client's cookie is dropped with `Max-Age: 0`. |
| `rotate()` | A new random id over the same data — the one place a session id is made. A privilege change owes its old id this: a login rotates, so the id the attacker guessed before the login stops working (§27.1's tour). |
| `copy()` | The snapshot a store keeps; the two per-request flags do not travel with it, and a copy is not dirty. |

The cookie the id travels in is named `nlang_session`, with the
`__Host-` prefix when `useSessions` asked for it. It is written
`HttpOnly` always (that is the session cookie's own rule, not a
choice the app gets), with `Path=/` and `SameSite=Lax`, `Secure` only
when the app's `secureCookies` switch says the app is served over
HTTPS, and no `Max-Age` (a browser-session cookie). The app writes exactly one
cookie name for sessions, so an app's own cookies and the framework's
never collide.

The write-back runs after the whole chain — after the error stage
too, so a session written by a handler that then threw is still saved
and its cookie still reaches the client. Its rules: a session that
was written, or whose id changed, is saved to the store and announced
with a cookie; a brand-new session that wrote nothing gets no cookie
(an id handed out for a request that stored nothing would be a cookie
for nothing); a request that only read its session reissues no cookie;
and an id the client still holds stops working the moment the session
rotates or destroys, because the store destroys it.

| Store | Notes |
| --- | --- |
| `SessionStore` | The three-member face a custom store implements — `load(id)` (the record, or `null` when gone or expired), `save(s)` (store it and refresh its expiry), `destroy(id)` — and the one type the framework talks to. One store serves every request of the app from every thread, so an implementation has to be safe to call concurrently (§10); a store's own failures surface as 500s like a broken handler's. |
| `MemorySessionStore()` | The store an app gets by asking for sessions: a locked table in this process. `MemorySessionStore(idleTtlMs)` and `MemorySessionStore(idleTtlMs, sweepIntervalMs)` replace the two defaults. |

The default lifetime is half an hour **without a write**: every
`save` slides the expiry forward, so a session an app keeps touching
stays alive, while an id nobody writes to any more does not stay
valid for a working day. A read-only request does not extend the
lifetime. Expired records are swept inside ordinary store calls — at
most one sweep a minute, and no background thread exists — and
`sweepExpired(long nowMs)` runs one sweep against a chosen instant,
which is how a test pins the expiry boundary without waiting. The
store keeps snapshots, never aliases: the record the next request is
handed shares nothing with the session this request's handler still
holds, and a custom store is built to keep snapshots too — the public
`Session()` constructor makes the empty record a store fills.

## 27.7 Static files

`StaticFiles` is a handler made from a directory, mounted under a
prefix:

```nlang
web.static.StaticFiles assets = web.static.StaticFiles("public");
assets.maxAgeSeconds = 3600;
app.mount("/static", assets.serve);
```

The constructor answers
`web.static.StaticFiles: root is not a directory: <root>` for a root
that does not exist yet — a deployment that starts before its assets
do has to create the directory first. The public fields are the
policy: `index` (the per-directory index file, `"index.html"` by
default), `maxAgeSeconds` (`0`, which means send no `Cache-Control`
at all), `useETag` (`true`) and `allowDotFiles` (`false` — a path
segment that starts with a dot is refused, so `/.well-known` needs
this switch on). `addType(ext, mime)` extends the extension table
(refused for an empty extension or media type:
`web.static.StaticFiles.addType: <ext|mime> is empty.`); the built-in
table covers the common document, image, font and script types, an
off-table extension is served as `application/octet-stream`, and a
`text/*` type gains `; charset=utf-8`.

What `serve` does with a request:

- Anything but `GET` or `HEAD` is a `405` with `Allow: GET, HEAD`.
- The percent-decoded path is walked segment by segment, and a
  segment that is `.` or `..`, holds a NUL or a path separator,
  carries a Windows drive prefix, or starts with a dot while
  `allowDotFiles` is off answers a `404` with the body
  `not found: <path>`. The walk is what keeps a URL-encoded `..` from
  reading above the mounted root.
- A directory reached without a trailing slash is answered `301` with
  `Location` set to the same path plus `/`, query string preserved;
  a directory reached with one serves its `index` file if present.
- The response carries `Last-Modified` (file modification time from
  `fs.modifiedTimeMs`, §16.3) and, when `useETag` is on, a weak
  entity tag (ETag) `W/"<size>-<mtimeMs>"` (§23.3). A later request
  may revalidate with `If-None-Match` or `If-Modified-Since`, and a
  match answers `304` with no body; when both conditions arrive, the
  tag wins, and a condition the parser cannot read counts as absent,
  which is the safe direction.

A file that vanishes between the check and the read surfaces as a
500 (§23.5) — the race a static server accepts rather than locking
its tree. Mount the root mount (`/`) last, so literal routes keep
priority (§27.5), and remember that a mounted subtree answers its own
misses: `app.mount("/", assets.serve)` means the app's
`setNotFound` hook never sees a static miss.

## 27.8 Server-sent events

`Sse` writes the event stream of a response the handler keeps open:

```nlang
void feed(web.RequestContext ctx) {
    web.sse.Sse sse = web.sse.Sse(ctx.res);
    sse.send("one");                 // data: one
    sse.send("tick", "two");         // event: tick, data: two
    sse.send("42", "tick", "two");   // id: 42 on top of the same pair
    sse.comment("still here");       // a ": still here" keep-alive line
    sse.close();                     // end the stream
}
```

The constructor takes the response, sets the headers the stream needs
(`Content-Type: text/event-stream`, `Cache-Control: no-cache`,
`Connection: keep-alive` and `X-Accel-Buffering: no` to keep a
proxy from buffering the first frames) and publishes a
`Channel<byte[]>` (§10.4) as the response's streaming body. Each
`send` writes one event: an optional `id:` line, an optional `event:`
line, one `data:` line per line of the payload (a payload with no
lines still sends one empty `data:` line), and the blank line that
ends it; `id:` and `event:` values that carry a carriage return (CR)
or line feed (LF) are refused outright
(`web.sse.Sse.send: id must not contain CR or LF.`, and the same
sentence naming `event`), because they would break the frame they are
written into, and an empty one omits its field line entirely. `comment(text)` writes each of its lines after a `: ` —
browsers ignore such lines, which makes them the keep-alive.

The channel is the backpressure and the cancellation point together
(§23.4): `send` blocks while a slow client has filled the buffer
(capacity 16 frames), and a client that hangs up closes the channel,
so the handler's next `send` raises `ChannelClosedException` (§10.4)
— its cleanup point, exactly once. `close()` closes the channel and
ends the stream; it may be called twice without complaint, and a
`send` after it raises the same `ChannelClosedException`. A stream
is never compressed: opening `compressBody` on a response that is
already streaming would compress a body nobody buffers (§23.6).

For the bare framing without this helper — a `Channel<byte[]>` and
the event bytes written by hand — `examples/http_sse.n` (the streaming
example of §23.4) is the reference; the `web` layer exists so the
handler writes events instead of bytes.

## 27.9 Forms and uploads

The `web.form` module reads both encodings a browser sends and builds
the multipart one on the client side. `Form.parse(ctx)` is the entry
point; `FormOptions` carries the three budgets — `maxBytes` (8 MiB of
body, both encodings), `maxParts` (1000) and `maxPartBytes` (4 MiB
each) — and `Form.parse(ctx, opts)` runs under a custom set. The
steps land in a fixed order: an empty body answers an empty form
before the content type is even consulted; a content type that is
neither `application/x-www-form-urlencoded` nor
`multipart/form-data` answers `415`
`web.form.Form.parse: unsupported content type for form parsing.`
without reading a body byte (the http layer drains it and the
connection stays reusable, §23.4); a malformed boundary parameter and
each grammar overrun answer their own named `400` or `413`:

| Verdict | Message shape |
| --- | --- |
| 413 over the body budget | `web.form.Form.parse: form body exceeds maxBytes (<n> bytes).` |
| 413 over the part count | `web.form.Form.parse: form exceeds maxParts (<n> parts).` |
| 413 over one part's body | `web.form.Form.parse: part exceeds maxPartBytes (<n> bytes).` |
| 400 missing, empty or repeated `boundary` parameter | `web.form.Form.parse: missing boundary parameter.` / `web.form.Form.parse: multiple boundary parameters in content type.` |
| 400 no delimiter line, or a body that never closes | `web.form.Form.parse: no boundary delimiter in multipart body.` / `web.form.Form.parse: missing closing boundary delimiter.` |
| 400 a part without a `form-data` disposition, without a `name`, or with a name past 100 bytes | `web.form.Form.parse: part missing form-data content-disposition.` / `web.form.Form.parse: part content-disposition has no name parameter.` / `web.form.Form.parse: field name exceeds 100 bytes.` |
| 400 a part's header block past 16384 bytes | `web.form.Form.parse: part headers exceed 16384 bytes.` |
| 400 a urlencoded value with a broken `%` escape | `web: malformed percent escape in form body` |

Every one of these is an `HttpException` (§23.8), so the app answers
it with exactly that status and message; the count budget outranks the
per-part verdicts, which is why a part over `maxParts` answers the
`413` even when its own headers would also have failed a `400`.

A parsed form is a `FormData`: names in first-appearance order, each
name mapped to its field values in arrival order and to its file
parts in arrival order (`names`, `get`, `getAll`, `getFile`,
`getFiles`, `files`, plus `has`, `fieldCount`, `fileCount` and
`partCount`). The read accessors hand out copies, so editing what a
handler received leaves the form untouched. A name carrying both
fields and files enters the name list once. The channel split follows
the `filename` parameter's presence: a part that spells `filename=""`
is a zero-named file part, not a field. A part's declared
`Content-Type` registers verbatim (the bare `text/plain` when the
part spelled none); the filename keeps its last path segment and
nothing else — browsers send `C:\fakepath\report.png`, and `filename`
answers `report.png` with no other cleaning. Multipart field values
stay exactly as sent (no percent decoding, unlike the urlencoded arm,
whose `+` and `%XX` decode as in chapter 22).

The two `add` methods of `FormData` are public too, so a caller can
assemble a form by hand: handler code written around the accessors
then runs unchanged over a parsed wire message and over a form the
program built itself. `addField(name, value)` appends a field value
under `name`; the name joins the name list at its first appearance,
exactly as parsing would. `addFile(file)` registers a `FormFile`
under the field name the file itself carries, appending it to that
name's file parts in arrival order. Neither makes a copy: `addField`
keeps the value as given, and `addFile` registers the `FormFile`
handle as given, so its `data` keeps whatever posture the caller
handed it (the copy-at-registration posture belongs to
`MultipartBody.addFile`, below).

A file part is a `FormFile`: `field`, `filename`, `contentType` and
`data` — a view onto the buffered body rather than a copy — with
`size()` and `text()`. Its save face is three members:

| Member | Notes |
| --- | --- |
| `safeName()` | The name cleaned for storage: control bytes out, whitespace runs folded to one space and the ends trimmed, a bare `.` or `..` dropped, the client's path stripped on both separators, a Windows device name (`con`, `nul`, `com1`, …) prefixed with `_`, and a truncation at 120 bytes that never cuts a Unicode Transformation Format (UTF-8) character in half. A name that cleaned away to nothing answers `upload`. The order of these steps is itself pinned behaviour: stripping the path before judging the device name is what turns `C:\fakepath\nul` into `nul` and then into `_nul`. |
| `saveTo(string dir)` | Write the bytes into an existing directory under `safeName()` plus a random 8-hex-character suffix spliced before the extension — two uploads that clean to the same name never collide. Nothing is created on the way: a missing directory answers `fs.writeBytes: cannot open "<path>".` (an `IOException`, §16.4). |
| `saveAs(string path)` | Write to an exact path verbatim: no cleaning, no suffix — the caller owns the name it chose. |

`MultipartBody` is the same message from the builder's side, for a
client posting to such an endpoint:

```nlang
web.form.MultipartBody mp = web.form.MultipartBody();
mp.addField("note", "hello web");
mp.addFile("doc", "report.txt", "text/plain", io.toBytes(text));
req.headers.set("Content-Type", mp.contentType());
req.body = mp.toBytes();
```

`addFile` copies its data at registration, so the caller may reuse
the buffer for the next message (the parsed `FormFile.data` is the
opposite posture, a view, because the body is already buffered).
A registered `Content-Type` that carries CR or LF is rejected rather
than escaped (a newline there would invent a header line), an empty
one sends `application/octet-stream`, and the three rejections all
land before anything registers, so a refused add leaves no half part
behind:

| Call | Rejection |
| --- | --- |
| `addField` with an empty name | `web.form.MultipartBody.addField: name must not be empty.` |
| `addFile` with an empty field | `web.form.MultipartBody.addFile: field must not be empty.` |
| `addFile` with a `contentType` carrying CR or LF | `web.form.MultipartBody.addFile: contentType must not contain CR or LF.` |

In the two quoted parameter values (`name` and `filename`) the
builder escapes exactly the quote and the two newline characters —
`"` becomes `%22`, CR `%0D`, LF `%0A` — and nothing else, backslash
and percent included. The parser does not undo those escapes, so a
round trip through `MultipartBody` and `Form.parse` returns a value
unchanged only when it carries no `"`/CR/LF; a value that carries one
arrives with it spelled in the `%` form.

The boundary itself is 40 lower-case hex characters from
`math.randomBytes` (§13.3), and the builder automatically picks a
boundary that is safe against part-body collisions: a part body
carrying `--<boundary>` at the start of a line would end the message
early (RFC 2046), so the builder re-rolls until none does (a
boundary too unlucky eight rolls in a row is a constructible
coincidence and answers as a named failure). The two calls that
assemble the message — `contentType()` and `toBytes()` — read and
may rewrite one resolved boundary, so they can never disagree, but
each may do the scanning work: calling `contentType()` can consume
body bytes looking for collisions and can change the boundary, and
parts added after a first `toBytes()` are scanned too, so a second
`toBytes()` can answer different bytes than the first did. Ask each question once
in order — add every part, then read `contentType()`, then
`toBytes()` — and the bytes and the header describe one message.

The shipped example `examples/web_upload.n` puts both sides in one
program: the client builds a form with `MultipartBody`, the server
reads it with `Form.parse` and saves the file with `saveTo`, and the
client then checks what landed on disk.

```nlang
// Example: a multipart upload round trip. The client builds a form with
// `MultipartBody`, the server reads it with `Form.parse` and saves the file
// under a sanitized name, and the client then checks what landed on disk.
import fs;
import http;
import io;
import url;
import web.*;

void upload(web.RequestContext ctx) {
    web.form.FormData form = Form.parse(ctx);
    web.form.FormFile doc = form.getFile("doc");
    if (doc == null) {
        ctx.text(400, "no file part");
        return;
    }
    doc.saveTo("uploads");            // sanitized name plus a random suffix (D-20)
    ctx.text(200, "saved " + doc.safeName() + " " + doc.size().toString() + " " + form.get("note"));
}

// The client side of one POST: only the multipart body changes between legs.
http.HttpResponse sendForm(http.HttpServer srv, web.form.MultipartBody form) {
    http.HttpRequest req = http.HttpRequest();
    req.method = "POST";
    req.url = Url.parse("http://127.0.0.1:" + srv.localPort().toString() + "/upload");
    req.headers.set("Content-Type", form.contentType());
    req.body = form.toBytes();
    return http.request(req);
}

int main() {
    fs.makeDirs("uploads");

    web.App app = web.App();
    app.post("/upload", upload);
    app.listen("127.0.0.1", 0);       // port 0 = an ephemeral one
    http.HttpServer srv = app.server();

    bool ok = true;
    string text = "line one\nline two\n";

    // A browser sends the whole path as filename; the server echoes only the
    // last segment and writes the file under an even safer name.
    web.form.MultipartBody mp = web.form.MultipartBody();
    mp.addField("note", "hello web");
    mp.addFile("doc", "C:\\fakepath\\report.txt", "text/plain", io.toBytes(text));
    io.print("ctype=" + mp.contentType().startsWith("multipart/form-data; boundary=").toString());
    ok = ok && mp.contentType().startsWith("multipart/form-data; boundary=");

    http.HttpResponse res = sendForm(srv, mp);
    io.print("status=" + res.statusCode.toString());
    io.print("body=[" + res.bodyText() + "]");
    ok = ok && (res.statusCode == 200);
    ok = ok && (res.bodyText() == "saved report.txt 18 hello web");

    // The saved file has a random suffix before its extension, so list the
    // directory instead of guessing the name.
    List<string> names = fs.listFiles("uploads");
    io.print("count=" + names.length().toString());
    string disk = fs.join("uploads", names[0]);
    io.print("suffixed=" + (names[0].startsWith("report-") && names[0].endsWith(".txt")).toString());
    io.print("bytes=" + fs.size(disk).toString());
    io.print("content=[" + io.fromBytes(fs.readBytes(disk)) + "]");
    ok = ok && (names.length() == 1);
    ok = ok && names[0].startsWith("report-") && names[0].endsWith(".txt");
    ok = ok && (fs.size(disk) == text.length());
    ok = ok && (io.fromBytes(fs.readBytes(disk)) == text);

    // A form with nothing but a field: the handler's "no file" branch.
    web.form.MultipartBody onlyField = web.form.MultipartBody();
    onlyField.addField("note", "no file");
    res = sendForm(srv, onlyField);
    io.print("nofile=" + res.statusCode.toString() + " [" + res.bodyText() + "]");
    ok = ok && (res.statusCode == 400);
    ok = ok && (res.bodyText() == "no file part");

    app.drain(5000);                  // let in-flight requests finish (D-22)
    app.close();

    int rc = 0;
    if (ok == false) {
        io.print("mismatch");
        rc = 1;
    } else {
        io.print("ok");
    }
    return rc;
}
```

The server half of that program is the five lines of `upload`: parse,
`getFile`, branch on `null`, `saveTo`, answer — the handler shape a
real upload endpoint keeps.

## 27.10 Limitations

- Uploads are buffered, not streamed: `Form.parse` reads the whole
  body (up to `maxBytes`) before parsing, because its `FormFile.data`
  is a view into that buffer and every accessor reads random. A
  streaming multipart reader would trade the view face for
  part-at-a-time callbacks, and it is not built yet.
- There is no WebSocket. The http layer hands an `Upgrade` header to
  the handler and never takes the connection over itself (§23.5), and
  the binary frame protocol on top would need that takeover first;
  long-lived one-way delivery is what SSE covers today (§27.8).
- There are no templates. Rendering markup from data is application
  work — the package stops at `ctx.html` and `ctx.bytes`, and a
  template language is a library above it, not a duty of the
  transport layer.

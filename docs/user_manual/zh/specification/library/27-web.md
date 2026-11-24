> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 27. web包

`web`包是建立在`http`包（第23章）之上的应用层：洋葱式的中间件链、每个请求一个上下文并携带处理器所需的全部应答帮手、一张路由表、会话、挂载的静态文件服务、服务器推送事件（SSE），以及对浏览器两种发送编码的表单解析。它不自持套接字——`App`包装一个`HttpServer`，传输层的框架（正文上限、超时、连接复用）仍归第23章记载。这个包用NLang本身写成，分四个模块：核心在`stdlib/web.n`，静态文件在`stdlib/web/static.n`，事件流在`stdlib/web/sse.n`，表单在`stdlib/web/form.n`。它不声明自己的异常类：被拒的请求以第23章的`HttpException`到来，传输故障以第21章的`SocketException`到来，框架误用则以普通`Exception`（第8章）到来。

四个模块拼成一个包。`import web;`只把包级模块绑定到名字`web`上；住在子模块里的类型还需要各自的导入——`import web.static;`、`import web.sse;`或`import web.form;`——或者一条`import web.*;`，它把包模块与其全部子模块合并，下面的导览用的正是这条。模块一旦进入作用域，其成员两种写法都应答：`Form.parse(ctx)`与`web.form.Form.parse(ctx)`是同一个调用，程序按可读性取舍即可。当一个模块里拼写的类型出现在另一模块书写的名字里——处理器签名写下`web.form.FormData`——模块限定要留在名字上。名为`web`的用户包与标准库相撞，被重复包规则拒绝（§11.1）。

一切塑造应答形态的东西在其第一个请求被处理的那一刻定型：`use`、`useSessions`与路由注册都在§27.2与§27.5里点名了那条拒绝。两个各只占一个槽位的钩子——`onError`与`setNotFound`——任何时刻都可更换，因为晚写一次只意味着此前的请求以另一种形态得到回答。包里还携带若干框架内部帮手（会话id与上传名共用的十六进制拼写、会话与属性之下的值袋、`use`背后的函数适配器）；它们在源码里可见，但不属于本章。

这个包声明：

- `App`，持有服务器的门面（§27.2）；
- `RequestContext`，每请求状态（§27.3）；
- `Middleware`，链上的一环（§27.4）；
- `Router`，路由表（§27.5）；
- `Session`、`SessionStore`与`MemorySessionStore`（§27.6）；
- `web.static.StaticFiles`，挂载的文件处理器（§27.7）；
- `web.sse.Sse`，事件流帮手（§27.8）；
- `web.form.Form`、`FormOptions`、`FormData`、`FormFile`与`MultipartBody`，表单面（§27.9）。

## 27.1 导览

导览即随附示例`examples/web_hello.n`；它启动服务器，用自己的客户端走遍每条路由，然后自行收尾，因此`main`可以返回、进程自然退出。

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

程序每段打印一行，以`ok`收尾。对照这份实录，各段正展示整个框架在工作：

- `nokey=401`——`requireKey`自己作答，从未调用`ctx.next()`，因此路由表压根没被查过（§27.4）。
- `item=item 7 query v=list`——`:id`路由捕获了那个段（由`param()`做百分号解码），并把原样到达的查询串交给处理器；`elapsed=true`是类中间件在`next()`返回后给响应盖章的头，包裹在路由处理器之外（§27.5）。
- `login=200 issued=true`——登录写入了会话并旋转了id，因此这条回答携带会话cookie`nlang_session=<id>`；`whoami=user dlite`把它读回，而`reissued=false`记录了一次只读会话的请求不再重发cookie（§27.6）。
- `anon=user -`——匿名访客拿到自己的全新空会话，而非陌生人的。
- `feed=true tail=true`——`Sse`帮手在`Content-Type: text/event-stream`上为两个事件分帧：正文以`data: one`帧开场，每个事件都以空行结尾（§27.8）。
- `static=200 <h1>hello web</h1>`与`missing=404`——挂载的`StaticFiles`处理器交出了目录索引，而挂载前缀之内的未命中是子树自己的404，不是应用的（§27.7）。
- `app.drain(5000)`等在途请求做完，`app.close()`交还监听套接字，`main`返回——退出码由`ok`算出（干净跑完就是`0`），这正是其他示例同样采用的自检形态（§9）。

注释里的字母编号（D-3、D-10、D-12、D-22）指认本包背后的设计决策；每条注释都写明了该决策定下的规则，编号只是把规则回溯到决策的线索。

## 27.2 App

`App()`就是整个门面：它收拢中间件链、把注册转发给唯一的路由表、持有那个`HttpServer`，并把链做的一切或抛出的一切变成单一响应。

| 注册面 | 说明 |
| --- | --- |
| `use(Middleware m)` | 追加一环链（§27.4）。 |
| `use(func<void, RequestContext> fn)` | 用普通函数写同一环链，适合小到不值得建类的环。 |
| `useSessions(SessionStore store, bool secureCookies = false, bool hostPrefix = false)` | 打开会话（§27.6）。两个开关就是会话cookie的`Secure`属性与`__Host-`名字前缀；只要前缀而不带`Secure`会当场被拒，因为浏览器对无前缀资格的cookie只会静默丢弃而非登记。 |
| `on(string method, string pattern, func<void, RequestContext> h)` | 注册一条路由；`get`、`post`、`put`、`patch`、`head`、`del`与`any`是动词简写（`del`是因为`delete`是关键字，与§23.7同理）（§27.5）。 |
| `mount(string prefix, func<void, RequestContext> h)` / `mount(string prefix, Router subtree)` | 把前缀之下的子树交给一个处理器或另一张路由表（§27.5）。 |
| `setNotFound(func<void, RequestContext> h)` | 应用自己对无人服务之路径的作答。 |
| `onError(func<void, RequestContext, Exception> h)` | 在普通500重建之后重塑失败响应（见下文）。 |

| 运行面 | 说明 |
| --- | --- |
| `listen(string host, int port)` | 启动服务器后即返回：accept循环在自己的线程上跑（§23.5），`main`得以继续——导览用的正是这个形态。port填`0`申请临时端口，`server().localPort()`报告选定值。 |
| `serve(string host, int port)` | 同一条accept循环跑在调用线程上，是独立服务器进程的入口，其`main`不得返回（§10.9）。 |
| `server()` | 被持有的那个`HttpServer`本尊：§23.5的正文上限与超时都在这个对象上设置，`isServing()`也在那里看守——`App`有意只转发`listen`、`serve`、`drain`与`close`，让一层传输面同时服务两层。 |
| `drain(int graceMs = 15000)` | 转发`HttpServer.drain`（§23.5）：停止接受，等在途请求做完，并回报宽限期耗尽时被强制关闭的连接数。可重复调用，未在运行的服务器返回`0`。 |
| `close()` | 转发`HttpServer.close`（§23.5）。此后应用还能再`listen`——路由表、链与会话毫发无损。 |

注册随第一个请求一起定型，因为「一个在跑的应用长什么样」只该有一个答案：`use`此后回答`web.App.use: middlewares must be registered before the first request.`，`useSessions`回答`web.App.useSessions: sessions must be configured before the first request.`，迟到的路由注册撞上§27.5的`web.Router`消息。`hostPrefix`不带`secureCookies`的`useSessions`即使在首个请求之前也回答`web.App.useSessions: the __Host- cookie prefix requires the Secure attribute.`。

链内的失败只受一种处置。`HttpException`（§23.8）以它自己的`status`作答，以它自己的消息作正文；status为`0`则钳到500。其他异常回答500、正文写`internal error`——解释器细节不外泄到线上——随后`onError`钩子若已设置便可重塑作答。钩子从朴素作答起步是深思熟虑的：抛出的链可能已经写了响应的一部分，而头部表被整体替换，因此抛出之前某中间件设下的`Content-Type`不会与它从未描述过的错误正文同车寄出。钩子自己再抛就仍是一眼可见的500，由http层再映射一次（§23.5）。

两种启动失败由下层来答，而`App`不去改它们的名字，因为`listen`与`serve`共用一条启动路径，同一个故障无论走哪扇门都保持同一个消息。重复启动已在运行的服务器回答`http.HttpServer.start: already started by this instance.`（经`serve`则同名换成`serve`）。端口被他进程占用则回答第21章的`SocketException`，即`net: bind <host>:<port> failed: <system text>`——这是调用方要接住的寻常结局，而失败的启动把应用留在未启动状态，改好端口再`listen`即可重来。

## 27.3 RequestContext

一个`RequestContext`服务一个请求：中间件与处理器都把它当唯一参数，而离开这个应用的唯一东西就是它最终承载的响应。处理器经由这些字段读请求：

| 字段 | 说明 |
| --- | --- |
| `req`、`res` | 第23章的`HttpRequest`与`HttpResponse`，下面应答帮手所写的那一对。 |
| `method` | 原样到达的请求方法。 |
| `path` | 原始、未解码的路径；路由表按这些字节匹配，解码是处理器自己的`param()`的事（§22、§27.5）。 |
| `query` | 原始、未解码的查询串；`url.percentDecode`与第22章的解析面按路由想要的方式读它。 |
| `params` | 路由表留下的捕获段；处理器经下面的`param()`读它们。 |

| 路由参数 | 说明 |
| --- | --- |
| `param(string name)` | 捕获的段，已百分号解码；畸形转义原样返回那些字节而非让请求失败，未被捕获的名字回答`""`。 |
| `paramInt(string name, int def = 0)` | 把同一个段按数字读；缺失的段与不成数的值都回答`def`。 |
| `hasParam(string name)` | 命中的模式是否捕获了这个名字。 |

| 属性 | 说明 |
| --- | --- |
| `set(string key, Object value)` / `has`、`remove` | 每请求键值存储，中间件把发现留给后续链环的地方（§27.4）。 |
| `get(string key, Object def = null)` | 存的值，或`def`。 |
| `getString`、`getInt`、`getBool` | 带类型的读取，默认分别回答`""`/`0`/`false`。`getInt`与`getBool`经由值自身的文本读取，因此以文本存的数字能按数字读回；读不成目标类型的值就是调用方的默认值，而不是错误。 |

| 会话 | 说明 |
| --- | --- |
| `session()` | 本请求的会话（§27.6），首次使用时从存储加载，之后缓存，因此一条链里每个环共享同一个对象。在从未调用`useSessions`的应用上伸手要它会回答`web.App.useSessions: no session store configured.` |

| 应答帮手 | 说明 |
| --- | --- |
| `text(int status, string body)` | `text/plain; charset=utf-8`（§14.5）。 |
| `html(int status, string body)` | `text/html; charset=utf-8`。 |
| `json(int status, JsonValue value)` | 第26章某个值的`serialize()`文本，类型`application/json`。 |
| `bytes(int status, byte[] body, string contentType)` | 任意媒体类型下的任意正文。 |
| `redirect(string location, int status = 302)` | `3xx`，目标在`Location`里，无正文。 |
| `noContent()` | `204`，丢掉写了一半的正文。 |

每个帮手都设置状态码、匹配的理由短语（§23.2）与正文；处理器之后仍可继续编辑`res`——帮手写响应，不发送响应。

| Cookie | 说明 |
| --- | --- |
| `cookie(string name)` | 请求`Cookie`头（§23.3）里该名字的第一个`name=value`对；值保持百分号编码原样，因为没有任何声明说过它被编码过。 |
| `setCookie(string name, string value, string path = "/", int maxAgeSeconds = 0, bool httpOnly = true, bool secure = false, string sameSite = "Lax")` | 追加一条`Set-Cookie`行（每条cookie需要自己的一行，头部因此累积）。`maxAgeSeconds`是三态的：`0`不写`Max-Age`（活到浏览器会话结束的cookie），正数发送它，负数发送`Max-Age: 0`、在客户端丢弃这条cookie。 |

`ctx.next()`推进链：运行下一环，链答完即返回（§27.4）。同一环推进两次是处理器缺陷——推进两次的环会把下游跑两遍，而响应只讲得通一次——因此它回答500`web: next() called more than once`。

## 27.4 Middleware

中间件就一个方法：

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

基类`process`的默认实现就是`ctx.next()`，所以只想包裹链其余部分的中间件在`next()`两侧写好自己的两半、其余全部继承——导览里的`Timing`类正是这个形态。从不叫`next()`的环是短路：它自己作答，后续环与路由表都不会再看见这个请求。普通函数不需要类：`use(fn)`把一个`func<void, RequestContext>`包进同一条链，导览因此能把`requireKey`与`Timing()`并排注册。

注册顺序即洋葱顺序：最先`use`的最先运行、最后返回，而路由表作为最后一环骑在链上。共享的中间件实例服务每一个请求，所以它不得在自己的字段里存每请求状态——那是§27.3属性存储的职责。

## 27.5 路由

模式是由三种段拼成的以`/`分隔的路径：字面段、恰好捕获一个段的`:name`、以及吞掉其下一切的结尾`*`。注册只是同一张表上的糖：

```nlang
app.on("GET", "/items/:id", showItem);   // same as app.get(...)
app.get("/search", search);              // literal
app.mount("/static", assets.serve);      // a subtree hands off
app.get("/api/*", apiCatchAll);          // param("*") holds the rest
```

表对请求的回答分两步。完全字面的模式在按注册顺序扫描之前先经哈希查中，因此字面路由无论之前注册了多少参数路由都保住自己的处理器；通配行之间，先注册者赢。找不到`HEAD`行的`HEAD`请求骑上`GET`行。路由回答哪些动词由注册决定：`get`注册`GET`，`any`注册`GET`、`HEAD`、`POST`、`PUT`、`PATCH`、`DELETE`与`OPTIONS`全部七个，而mount行回答全部七个——前缀之下的动词问题由子树自己裁决。

未命中是一种回答，不是异常。若这条路径在其他动词下注册过，应用回答`405`，附一行`Allow`列出那些动词（`HEAD`写在其所骑`GET`的旁边），正文写`method not allowed: <method>`；否则若有`setNotFound`钩子由它作答，默认是一记朴素`404`、正文写`not found: <path>`。挂载子树同样自报未命中：`Router.serve`就是子树对不服务于它的请求的回答，这正是`/static/`之下未命中能以子树自己的表发声的原因（§27.7）。405同样跳过`setNotFound`——表以别的动词认得的路径是动词错误，不是未知路径。

`mount(prefix, subtree)`把前缀剥掉后交给子树：在`mount("/static", ...)`之下，`/static/img/logo.png`以`/img/logo.png`抵达子树，恰等于前缀的请求以`/`抵达它（挂在根上则什么都不剥）。前缀一旦匹配，子树全权处置这条请求——它的404是它自己的，外层表再也不会看见这条请求。请求路径按原样匹配：结尾斜杠自成一个段，所以`/x/`与`/x`是两个路径，且只有`param()`解码。

注册正是犯错被抓住的地方，而被拒的注册让表保持原样：

| 拒绝 | 消息形态 |
| --- | --- |
| 同一路径同一动词注册两次，或一行已被更早一行完全遮蔽 | `web.Router: /a/:x conflicts with /a/b (registered first)`——永远不会轮到的那行被拒。 |
| 畸形模式 | `web.Router: invalid pattern "<pattern>": <reason>`——reason点名错处：空模式、缺开头`/`、结尾`/`、空段、`*`不在最后、`:`段没有名字、同一个段名用了两次、以及不够字面的mount前缀。 |
| 首个请求之后的注册 | `web.Router: routes must be registered before the first request.`——被请求回答过的表不再生长（§27.2）。 |

## 27.6 Sessions

会话是自愿参加的：`useSessions`把一个存储交给应用，处理器经`ctx.session()`抵达本请求的会话。数据存在服务器上；客户端只持有一个id。

| `Session`成员 | 说明 |
| --- | --- |
| `id` | 32个小写十六进制字符——16个随机字节（`math.randomBytes`，§13.3）拼成十六进制——这让猜id毫无价值。 |
| `isNew` | 本请求是否是这个会话为之初建的那一个。 |
| `isExpired` | 客户端出示的id是否已死：过期、伪造或已被销毁。登录页正是凭这个旗标显示「会话已超时」。 |
| `expiresAtMs` | 第15章单调时钟上的过期时刻，每次`save`都刷新；销毁后为`-1`。 |
| `set`、`has`、`get`、`getString`、`getInt`、`remove` | 数据本身，读取规则与默认值同§27.3的请求属性。 |
| `isDirty()` | 本次请求是否写入了数据；它决定存储是否要吃一次`save`。 |
| `destroy()` | 立刻忘掉这个会话：数据清空，服务器记录移除，客户端的cookie以`Max-Age: 0`丢弃。 |
| `rotate()` | 同样的数据换一个新的随机id——会话id唯一的生产地。身份提升欠旧id这一笔：登录时旋转，攻击者在登录前猜中的id就此失效（见§27.1导览）。 |
| `copy()` | 存储要留的快照；两个每请求旗标不随行，且副本不算脏。 |

id旅行所乘的cookie名为`nlang_session`，`useSessions`要了前缀则加`__Host-`前缀。它总是写`HttpOnly`（这是会话cookie自身的规矩，不是应用可选的余地），配`Path=/`与`SameSite=Lax`，只有应用的`secureCookies`开关声明应用经HTTPS对外时才加`Secure`，且不写`Max-Age`（浏览器会话级cookie）。应用为会话只写这一个cookie名，因此应用自己的cookie与框架的永不相撞。

写回跑在整条链之后——错误阶段之后也跑，因此一个「先写会话、随后抛出」的处理器所写的会话照样会被保存，cookie照样抵达客户端。规则如下：写过数据或id变了的会话，存入存储并以cookie宣告；全新但什么都没写的会话拿不到cookie（为一个什么也没存的请求发id，等于为虚无发cookie）；只读了会话的请求不重发cookie；而只要会话旋转或销毁，存储移除记录，客户端手里那个id即刻失效。

| 存储 | 说明 |
| --- | --- |
| `SessionStore` | 定制存储要实现的那个三成员面——`load(id)`（记录，失踪或过期则`null`）、`save(s)`（存下并刷新期限）、`destroy(id)`——也是框架唯一对话的类型。一个存储从每个线程服务应用的每个请求，实现必须并发安全（§10）；存储自身的失败与坏处理器一样浮成500。 |
| `MemorySessionStore()` | 开口要会话就能拿到的存储：本进程里一张加锁的表。`MemorySessionStore(idleTtlMs)`与`MemorySessionStore(idleTtlMs, sweepIntervalMs)`替换两个默认值。 |

默认寿命是「无写入半小时」：每次`save`都把期限向前滑，因此应用持续触碰的会话始终活着，而没人再写的id不会赖到第二个工作日。只读请求不延长寿命。过期记录在寻常的存储调用里顺带清扫——每分钟至多一次，不存在后台线程——`sweepExpired(long nowMs)`对指定时刻跑一次清扫，测试因此不必干等就能钉住过期边界。存储存的是快照而非别名：下一个请求拿到的记录与本请求处理器手里那个会话不共享任何东西；定制存储也应照此存快照——公开的`Session()`构造器就是给存储填充空记录用的。

## 27.7 静态文件

`StaticFiles`是由一个目录做成的处理器，挂载在某个前缀下：

```nlang
web.static.StaticFiles assets = web.static.StaticFiles("public");
assets.maxAgeSeconds = 3600;
app.mount("/static", assets.serve);
```

构造器对尚不存在的根回答`web.static.StaticFiles: root is not a directory: <root>`——先于素材启动的部署得先建那个目录。公开字段就是策略：`index`（每目录的索引文件，默认`"index.html"`）、`maxAgeSeconds`（`0`，即完全不发`Cache-Control`）、`useETag`（`true`）与`allowDotFiles`（`false`——以点开头的段被拒绝，所以要服务`/.well-known`就得打开这个开关）。`addType(ext, mime)`扩充扩展名表（空扩展名或空媒体类型会被拒：`web.static.StaticFiles.addType: <ext|mime> is empty.`）；内置表覆盖常见的文档、图片、字体与脚本类型，表外扩展按`application/octet-stream`寄出，`text/*`类型追加`; charset=utf-8`。

`serve`对请求做的事：

- 除`GET`与`HEAD`之外一切回答`405`，附`Allow: GET, HEAD`。
- 百分号解码后的路径逐段行走，段为`.`或`..`、含NUL或路径分隔符、带Windows盘符前缀、或在`allowDotFiles`关着时以点开头的，一律回答正文为`not found: <path>`的`404`。这场行走正是编码过的`..`读不到挂载根之上内容的原因。
- 未带结尾斜杠的目录回答`301`，`Location`设为同一路径补上`/`、查询串原样保留；带结尾斜杠到达的目录，若有`index`文件就服务它。
- 响应携带`Last-Modified`（文件修改时间取自`fs.modifiedTimeMs`，§16.3），`useETag`开着则附一个弱实体标签（ETag）`W/"<size>-<mtimeMs>"`（§23.3）。后续请求可用`If-None-Match`或`If-Modified-Since`再验证，命中即`304`且无正文；两个条件同至时标签获胜，解析器读不懂的条件按缺席处理——这是安全的方向。

文件在检查与读取之间蒸发会以500现形（§23.5）——这是静态服务器宁可接受也不给整棵树加锁的竞态。挂根（`/`）要挂得最晚，好让字面路由保持优先（§27.5），并记住挂载子树自报未命中：`app.mount("/", assets.serve)`意味着静态未命中永远见不到应用的`setNotFound`钩子。

## 27.8 服务器推送事件

`Sse`为一个保持打开的响应写事件流：

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

构造器接过响应，设置事件流所需的头部（`Content-Type: text/event-stream`、`Cache-Control: no-cache`、`Connection: keep-alive`，以及防止代理缓冲首批帧的`X-Accel-Buffering: no`），并把一个`Channel<byte[]>`（§10.4）发布为响应的流式正文。每次`send`写一个事件：可选的`id:`行、可选的`event:`行、正文每一行一条`data:`行（没有行的正文也发一条空的`data:`行），最后是收尾空行；带回车（CR）或换行（LF）的`id:`与`event:`值被当场拒绝（`web.sse.Sse.send: id must not contain CR or LF.`，`event`同样有这句点名），因为它们会切断自己置身其中的帧；为空的值整行省略。`comment(text)`把每一行冠以`: `——浏览器忽略这类行，它们因此就是保活手段。

通道同时是背压与取消点（§23.4）：慢客户端灌满缓冲（容量16帧）期间`send`阻塞；客户端挂断即关闭通道，于是处理器的下一次`send`抛起`ChannelClosedException`（§10.4）——那就是它的清理点，且只此一次。`close()`关闭通道、终结事件流；重复调用毫不抱怨，其后的`send`抛同一句`ChannelClosedException`。事件流永不压缩：对一个已经在流式发送的响应打开`compressBody`，等于压缩一个没人缓冲的正文（§23.6）。

至于不用这个帮手的裸分帧——`Channel<byte[]>`加手写的事件字节——参照`examples/http_sse.n`（§23.4的流式示例）；`web`层的存在就是为了让处理器写事件而不是写字节。

## 27.9 表单与上传

`web.form`模块既能读浏览器发送的两种编码，也能在客户端这一侧组装multipart消息。入口是`Form.parse(ctx)`；`FormOptions`携带三道预算——`maxBytes`（正文总量8 MiB，两种编码通用）、`maxParts`（1000）与`maxPartBytes`（单个4 MiB）——`Form.parse(ctx, opts)`则在自定义预算下运行。步骤按固定顺序落地：空正文在咨询内容类型之前就回答空表单；既非`application/x-www-form-urlencoded`亦非`multipart/form-data`的内容类型在没读一个正文字节的情况下回答`415`的`web.form.Form.parse: unsupported content type for form parsing.`（http层随后排空正文、连接得以复用，§23.4）；畸形的boundary参数与每种文法越限各自回答自己那句名点名的`400`或`413`：

| 判决 | 消息形态 |
| --- | --- |
| 正文越预算（413） | `web.form.Form.parse: form body exceeds maxBytes (<n> bytes).` |
| 部件数越限（413） | `web.form.Form.parse: form exceeds maxParts (<n> parts).` |
| 单部件正文越限（413） | `web.form.Form.parse: part exceeds maxPartBytes (<n> bytes).` |
| `boundary`参数缺失、为空或重复（400） | `web.form.Form.parse: missing boundary parameter.` / `web.form.Form.parse: multiple boundary parameters in content type.` |
| 没有分隔行，或正文迟迟不收尾（400） | `web.form.Form.parse: no boundary delimiter in multipart body.` / `web.form.Form.parse: missing closing boundary delimiter.` |
| 部件没有`form-data`处置行、没有`name`，或名字超100字节（400） | `web.form.Form.parse: part missing form-data content-disposition.` / `web.form.Form.parse: part content-disposition has no name parameter.` / `web.form.Form.parse: field name exceeds 100 bytes.` |
| 部件头部块超16384字节（400） | `web.form.Form.parse: part headers exceed 16384 bytes.` |
| urlencoded值里有坏掉的`%`转义（400） | `web: malformed percent escape in form body` |

这些全都是`HttpException`（§23.8），因此应用就以那个状态与那句消息作答；计数预算凌驾于单部件判决之上——部件越过`maxParts`时即便它自己的头部也会撞出一句`400`，回答仍是那记`413`。

解析完的表单是一个`FormData`：名字按首现顺序，每个名字映射到按到达顺序的字段值与按到达顺序的文件部件（`names`、`get`、`getAll`、`getFile`、`getFiles`、`files`，外加`has`、`fieldCount`、`fileCount`与`partCount`）。读取访问器交出的都是副本，处理器改拿到手的东西动不了表单本身。一个名字同时带字段与文件时只进名字表一次。频道归属跟着`filename`参数的存在与否走：写了`filename=""`的部件是个无名文件部件，不是字段。部件申报的`Content-Type`原样登记（部件没写则登记裸的`text/plain`）；filename保留最后一段路径且仅此而已——浏览器发的是`C:\fakepath\report.png`，`filename`回答`report.png`，此外不作任何清洗。multipart字段值一字不变（不做百分号解码，与urlencoded分支不同——那一侧的`+`与`%XX`如第22章所述解码）。

一个文件部件是`FormFile`：`field`、`filename`、`contentType`与`data`——已缓冲正文上的一个视图而非拷贝——外加`size()`与`text()`。它的落盘面有三个成员：

| 成员 | 说明 |
| --- | --- |
| `safeName()` | 为落盘清洗过的名字：控制字节出局，连续空白折成单个空格并剃掉两端，光秃的`.`或`..`丢弃，客户端路径在两种分隔符上都被剥掉，Windows设备名（`con`、`nul`、`com1`……）冠以`_`前缀，并在120字节处截断且绝不把一个Unicode转换格式（UTF-8）字符拦腰斩断。清洗后化为空无的名字回答`upload`。这些步骤的顺序本身就是钉死的行为：先剥路径再判设备名，才让`C:\fakepath\nul`变成`nul`再变成`_nul`。 |
| `saveTo(string dir)` | 把字节写进一个已存在的目录，文件名取`safeName()`加一个随机的8个十六进制字符后缀、插在扩展名之前——清洗后同名的两个上传永不相撞。途中什么都不创建：目录缺失回答`fs.writeBytes: cannot open "<path>".`（一个`IOException`，§16.4）。 |
| `saveAs(string path)` | 原样写到精确路径：不清洗、不加后缀——调用方认领自己挑的名字。 |

`MultipartBody`是同一条消息从构建者一侧的样子，供向这类端点投递的客户端使用：

```nlang
web.form.MultipartBody mp = web.form.MultipartBody();
mp.addField("note", "hello web");
mp.addFile("doc", "report.txt", "text/plain", io.toBytes(text));
req.headers.set("Content-Type", mp.contentType());
req.body = mp.toBytes();
```

`addFile`在登记时就拷贝其数据，调用方因此可以放心复用那只缓冲去装下一条消息（解析来的`FormFile.data`姿态相反——是视图，因为正文早已缓冲）。登记的`Content-Type`里带CR或LF会被拒绝而不是被转义（那里的一个换行会凭空捏造一行头部），空的发送`application/octet-stream`，而这三个拒绝都落在任何登记之前，被拒的add不会留下半个部件：

| 调用 | 拒绝 |
| --- | --- |
| `addField`名字为空 | `web.form.MultipartBody.addField: name must not be empty.` |
| `addFile`字段为空 | `web.form.MultipartBody.addFile: field must not be empty.` |
| `addFile`的`contentType`带CR或LF | `web.form.MultipartBody.addFile: contentType must not contain CR or LF.` |

两个带引号的参数值（`name`与`filename`）里，构建者恰好只转义引号与两个换行字符——`"`成`%22`、CR成`%0D`、LF成`%0A`——其余一切原样，反斜杠与百分号也在其列。解析器不会撤销这些转义，因此经由`MultipartBody`与`Form.parse`的往返只有在值不含`"`/CR/LF时才原样返回；含有其一的，到达时以`%`拼写现身。

boundary本身是`math.randomBytes`（§13.3）给出的40个小写十六进制字符，而构建器会自动挑选一个对部件正文冲撞安全的boundary：部件正文在某行行首携带`--<boundary>`会把消息提前截断（RFC 2046），于是构建器反复重掷直到无人如此（连续八掷都不走运的boundary是构得出来的巧合，会以一句点名失败作答）。组装这条消息的两个调用——`contentType()`与`toBytes()`——读写同一个已敲定的boundary，因此彼此永不矛盾，但每个都可能承担扫描工作：调用`contentType()`会消耗正文字节去找冲撞、也可能换掉boundary，而首次`toBytes()`之后追加的部件也会被再次扫描，第二次`toBytes()`因此可能答出与第一次不同的字节。按序只问一次——先加完所有部件，再读`contentType()`，最后`toBytes()`——字节与头部就描述同一条消息。

随附示例`examples/web_upload.n`把两侧装进同一个程序：客户端用`MultipartBody`构造表单，服务器用`Form.parse`读取并用`saveTo`保存文件，客户端随后核对盘上落了什么。

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

那个程序的服务器半边就是`upload`的五行：解析、`getFile`、判`null`分支、`saveTo`、作答——一个真正的上传端点会保持的处理器形态。

## 27.10 局限

- 上传是缓冲而非流式：`Form.parse`在解析之前把整个正文（至`maxBytes`）读进内存，因为它的`FormFile.data`是那块缓冲里的视图，且每个访问器都随机读取。流式的multipart读取器要拿视图面去换逐部件回调，这尚未建造。
- 没有WebSocket。http层把`Upgrade`头交给处理器、自己从不接管连接（§23.5），而叠在其上的二进制分帧协议需要先有这个接管；如今覆盖长寿命单向投递的是SSE（§27.8）。
- 没有模板。从数据渲染标记是应用的工作——本包止步于`ctx.html`与`ctx.bytes`，模板语言是它之上的一个库，不是传输层的职责。

> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 23. http包

`http`包说HTTP/1.1（RFC 9112）：一个服务器与一个客户端，构建在`net`包的传输控制协议（TCP）类之上（第21章）。它负责线格式——请求与响应解析、正文分帧、加固规则——而不管上层：路由、中间件、session与模板属于架在本包之上的web框架层。整包纯`nlang`源码（`stdlib/http.n`），没有自己的`native`成员；用到的标准库包只有`net`、`url`（第22章）与`gzip`（第24章）。

四件事有意划在界外，各自给出理由。传输层安全协议（TLS）：`net`包不提供，故`https`URL在打开套接字之前就被拒绝，消息说明原因。请求流水线：一条连接同一时刻只承载一个请求，与现代HTTP/1.1客户端一致——并发来自复用多条连接，不是在一条连接上交错请求。WebSocket升级：`Upgrade`头原样抵达处理器，本包不接管连接。一条连接的双向复用：客户端把请求正文全部写完，才开始读响应。

本包声明：

- `HttpRequest`与`HttpResponse`，消息对象（§23.2）；
- `HttpHeaders`，大小写无关的多值头映射（§23.3）；
- `HttpBodyReader`，流式正文背后的分帧感知读取器（§23.4）；
- `HttpServer`，一条accept线程加每连接一条线程（§23.5）；
- `HttpClient`，按源站划分的连接池，带重定向与超时（§23.6）；
- `HttpException`，失败类，失败带有HTTP状态时携带该状态（§23.8、§8.3）；
- 自由函数`http.get`、`http.post`、`http.put`、`http.del`、`http.request`与`http.defaultClient`（§23.7）。

用户包若命名为`http`将与标准库冲突，按重复包规则被拒绝（§11.1）。

## 23.1 同一进程内的服务器与客户端

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

程序打印`200 hello, world`与`echo: round trip`，只在两个应答都正确时以23退出（§9的自检写法）。它展示了一次完整往返的全部要素：

- 处理器是一个接收`HttpRequest`、返回`HttpResponse`的函数——类型`func<HttpResponse, HttpRequest>`。如上文的顶层函数`handle`、绑定方法、或该类型的变量，都是处理器。
- `start(host, port, handler)`绑定监听套接字、启动accept线程后返回，于是同一进程还能当客户端。端口`0`请求任意空闲端口，随后`localPort()`（连同`localAddress()`）报出实际选定的端口——本例因此无需占用固定端口。
- `http.get`与`http.post`是§23.7的自由函数，跑在共享客户端上。请求正文是`byte[]`；`io.toBytes`按Unicode转换格式（UTF-8）把文本编成字节（§14.5），`bodyText()`再解码回来。
- `srv.close()`释放监听套接字。要给别的机器用的服务器绑定`0.0.0.0`而非`127.0.0.1`；独立服务器进程调用`serve(...)`而非`start(...)`，因为`main`返回即进程结束（§10.9）。

随附示例在同一形态上展开：`examples/http_hello.n`跨多个请求保持连接存活，`examples/http_conc_bench.n`用一个客户端驱动一千个并发工作者，`examples/http_sse.n`推送事件流并在客户端挂断时停止生产者。

## 23.2 HttpRequest与HttpResponse

两个消息对象都是纯数据：发送前自己填的字段，与接收后由库填好的字段。

| `HttpRequest`字段 | 说明 |
| --- | --- |
| `method` | 构造时为`"GET"`。服务器从线上读入填充：凡合法token皆被接受，方法名区分大小写（§23.5）。 |
| `url` | 一个`Url`（第22章）。客户端侧：必须是绝对的且scheme为`http`，否则调用在连接之前抛`HttpException`。服务器侧：解析后的请求目标——origin形式、绝对形式与`*`都可读。 |
| `version` | 构造时为`"HTTP/1.1"`；解析器存下对端声明的版本。 |
| `headers` | 由构造函数创建的空`HttpHeaders`映射（§23.3）。 |
| `body` | 客户端侧：缓冲的请求正文，由库计算并发出`Content-Length`。与`bodyChannel`互斥。服务器侧：恒为`null`——处理器通过`bodyReader`读请求正文。 |
| `bodyChannel` | 客户端侧：以chunked流上传的`Channel<byte[]>`（§23.4）。与`body`互斥。 |
| `bodyReader` | 服务器侧：分帧感知的请求正文读取器（§23.4）。客户端侧：由`open()`设置，缓冲的`request()`之后为`null`。 |
| `bodyText()` | 以UTF-8解码的正文，无论正文在`body`还是`bodyReader`里都可用；无正文时为`""`。 |
| `connectionClose` | 解析出的`Connection: close`（请求），或你希望的回答（响应）：在响应上置位即在应答之后关闭连接。 |
| `framingMode`、`framingLength` | 解析器对正文分帧的判断——`HttpParser.MODE_NONE`、`MODE_LENGTH`、`MODE_CHUNKED`或`MODE_EOF`，模式为`MODE_LENGTH`时附带`Content-Length`。想要自己推理分帧的处理器读这两个字段；读`bodyReader`的处理器不需要。 |

| `HttpResponse`字段 | 说明 |
| --- | --- |
| `statusCode`、`reasonPhrase` | 构造时为`200`与`"OK"`。库为自己产生的状态给出理由短语（§23.5）；你自造的状态在你设置短语之前保持`"OK"`，这无妨——短语仅供参考，对端读的是`statusCode`。 |
| `version` | 构造时为`"HTTP/1.1"`；解析器存下对端声明的版本。 |
| `headers` | 一个`HttpHeaders`映射（§23.3）。 |
| `body` | 缓冲正文，随库计算的`Content-Length`发出。`null`表示无正文。状态低于200、204或304一律不带正文发出，无论此字段装了什么；`HEAD`请求得到长度而得不到字节。 |
| `bodyChannel` | 一个`Channel<byte[]>`，其中的块以chunked流出，带背压（§23.4）。 |
| `bodyReader` | 仅客户端侧：`open()`交回的流式正文。 |
| `requestUrl` | 该应答最终对应的URL——重定向之后（§23.6）为最后一跳；服务器侧为回显的请求目标。 |
| `compressBody` | 服务器侧：为`true`且客户端接受gzip时，缓冲正文被压缩，并设置`Content-Encoding: gzip`与`Vary: Accept-Encoding`（§23.6）。 |
| `connectionClose` | 为`true`即在此应答之后关闭连接。 |
| `bodyText()` | 与请求侧相同的「读取器或缓冲」逻辑；空正文返回`""`。 |

`Content-Type`从不替你凭空发明：空响应没有`Content-Type`，§23.7的自由函数只为自己构造的正文设置`text/plain; charset=utf-8`。

## 23.3 HttpHeaders

| 成员 | 说明 |
| --- | --- |
| `get(name)` | `name`的首个值，缺失时为`""`。名字大小写无关，故`get("content-type")`能找到`Content-Type`。 |
| `getAll(name)` | `name`的全部值，按收到的顺序；缺失时为空`List`。`Set-Cookie`以及对端可能重复的任何头都要用它。 |
| `set(name, value)` | 用这一个值替换`name`的全部值。 |
| `add(name, value)` | 追加第二个值，保留已有的。 |
| `remove(name)` | 丢弃`name`的全部值。 |
| `has(name)` | 名字是否存在，无论其值为何。 |
| `names()` | 不重复的名字，各按首次写入的拼写。 |
| `count()` | 名字/值条目数。 |
| `copy()` | 独立映射——客户端每次尝试用一个，重定向便无法留下被改写过的头。 |

`set`与`add`在名字或值含有裸回车符或换行符时抛`HttpException`，因为任一个都会把头部在线切成两段。对端把多值头折成一行（`Accept: gzip, deflate`）时由你自己拆：解析器按发送原样交回值，`HttpParser.splitTokens(value)`按逗号拆开。

## 23.4 正文：分帧由包代劳

消息正文有四种分帧方式：`Content-Length`计数、chunked流、直到连接关闭、以及无正文。发送时库选定分帧，接收时库识别分帧；`HttpBodyReader`把四者呈现为同一个字节源，处理器与客户端代码因此不必按分帧分支。

| `HttpBodyReader`成员 | 说明 |
| --- | --- |
| `read(buf, timeoutMs = 0)` | 把接下来的正文字节复制进`buf`并返回个数；正文结束后返回`-1`。返回少于请求的字节数是常态——一次调用可能只带回一部分。`timeoutMs <= 0`回落到客户端的`readTimeoutMs`或服务器的`bodyTimeoutMs`；期限已过抛`TimeoutException`（§20）。 |
| `readAll()` | 整个正文作为一个`byte[]`。越过大小上限（`HttpServer.maxBodyBytes`，§23.5）抛携带状态413的`HttpException`。 |
| `bodyText()` | `readAll()`按UTF-8解码。 |
| `close()` | 提前停止。未读尽的正文不能留在复用的连接上——剩下的字节会被误认作下一个响应——故该连接被关闭而不复用。 |
| `drain(maxBytes)` | 消费剩余部分以便复用连接；剩余超过`maxBytes`时返回`false`，即关闭的信号。服务器在每次调用处理器之后执行它。 |

**流式响应。** 处理器把`bodyChannel`设为一个`Channel<byte[]>`（§10.4）而非`body`，正文便以chunked流出。客户端读得慢时`send`阻塞，这就是背压：生产者跑到套接字前面的距离不会超过通道容量。关闭通道即写出结束块；`HEAD`请求不泵送而直接关闭通道，因此阻塞在`send`里的生产者也能收场。给`HTTP/1.0`客户端的流式应答退化为由连接关闭来分帧的原始字节，因为chunked是HTTP/1.1的分帧方式（RFC 9112 §9）。

**取消逆向传播。** 客户端在流中途断开，服务器下一次写入失败，库关闭通道，生产者下一次`send`抛`ChannelClosedException`——这正是处理器自己的清理点。没有任何轮询在探测对端是否已死。

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
    res.bodyReader.close();      // 提前挂断：该连接不回池

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

这是一个端到端的服务器推送事件（SSE）端点：处理器立即返回，生产者线程向通道供数据，客户端一次一个`read`地取事件，提前挂断即令生产者止步。事件分帧——`data:`行以空行分隔——在此故意手写：本包只认chunked流，不懂event-stream语义，那归web框架层。`read`循环是流式正文的诚实形态：字节成段到达，因此消息边界要在不断增长的累加器里查找，而非假设每次调用一条。

**流式上传。** 在请求上设置`bodyChannel`即以chunked发送，生产者侧同样是阻塞行为。流式上传边写边被消费，因此保留方法的重定向（307或308）无法重放它：这样的调用抛`HttpException`，而不是静默发出空正文（§23.6）。

## 23.5 HttpServer

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `maxHeaderBytes` | 65536 | 头部块大于此值：431。 |
| `maxHeaders` | 100 | 头字段数多于此值：431。 |
| `maxBodyBytes` | 16777216 | 交给处理器正文读取器的上限；越过的`readAll`抛`HttpException`413（§23.4）。 |
| `keepAliveTimeoutMs` | 30000 | 空闲连接在两个请求之间可等待多久。请求首字节到来之前的沉默会关闭连接，且不作答。 |
| `headerTimeoutMs` | 10000 | 请求行与头部的总时限，从首字节起算：一点一点挤字节的慢客户端在时限耗尽后得到408。单次读取不会等得更久。 |
| `bodyTimeoutMs` | 10000 | 处理器正文读取器每次读取的时限。 |
| `writeTimeoutMs` | 10000 | 响应每次写入的时限。大正文分片写出，故时限按片而非按整条消息计。 |
| `maxConnections` | 0 | `0`为不限。超过上限时新连接得到503并被关闭；这样的作答最多并发64个，再多出的连接直接关闭而不作答，客户端读到空响应。 |
| `maxRequestsPerConnection` | 0 | `0`为不限。第N条响应之后同时关闭连接。 |

`start(host, port, handler)`在套接字开始监听后即返回；`serve(host, port, handler)`在调用线程上跑同一条accept循环，是独立服务器的入口，其`main`不得返回（§10.9）。`close()`停止接受并唤醒阻塞中的`accept`；已在处理的连接在自己的keep-alive循环里把手头的请求做完。

accept循环每连接派生一条线程（§10），每条线程按顺序服务自己那条连接上的请求。处理器因此与其他处理器并发运行，且可以阻塞——等慢客户端、等`Channel`、等`Mutex`——而不会拖住整个服务器。没有线程池需要调大小：连接数即并发度，`maxConnections`就是封顶的旋钮。

服务器不问处理器就自行做的事：

| 状态 | 由谁产生 |
| --- | --- |
| 400 | 畸形的请求行、方法、请求目标或头行；在应当成对出现回车与换行符（CRLF）之处出现裸CR或裸LF；头部中的控制字符；冒号前的空白；不是token的名字；`Host`缺失或重复；`Content-Length`与`Transfer-Encoding`同时出现；`chunked`以外的`Transfer-Encoding`或不止一个`chunked`；畸形或互相冲突的`Content-Length`；畸形的块长度。 |
| 408 | 头部未在头部阶段时限内收完。 |
| 413 | 处理器的`readAll`或`bodyText`越过`maxBodyBytes`。 |
| 417 | 服务器不实现的`Expect`头。已知的`100-continue`在读正文之前先以`100`中间响应回答，客户端跳过中间响应。 |
| 431 | 头部块超过`maxHeaderBytes`或`maxHeaders`。 |
| 500 | 处理器抛出不带状态的异常，或返回`null`。正文写`internal error`：NLang内部细节不外泄到线上。 |
| 503 | 连接数达到`maxConnections`。 |
| 505 | `HTTP/1.1`与`HTTP/1.0`之外的请求版本。 |

这些回答一律关闭连接，因为解析失败后流位置不可知。处理器自己抛的`HttpException`是例外：其`status`字段指明状态时即用作回答（§23.8）。

头部是增量解析的，因此对端把一个请求拆成若干段、或把两个请求粘进同一段，都无需特判即可处理。`HTTP/1.0`请求在应答之后关闭，除非它要求`Connection: keep-alive`；`HTTP/1.1`请求一直存活，直到`Connection: close`或空闲超时。`Upgrade`头（含`websocket`）留在`headers`里交给处理器处置，连接不被升级。

写出时服务器补上处理器留给它的东西：响应没有`Date`时补一个；缓冲正文补`Content-Length`（无正文时为`0`）；流式正文补`Transfer-Encoding: chunked`；以及回答本身蕴含的`Connection`头——包括`HTTP/1.0`请求需要的那条`keep-alive`回显。有序关闭比看上去重要：服务器半关闭并让对端收尾，因为对端尚有字节排队时关闭套接字，可能以复位收场，把客户端还没读的应答一并丢掉。

## 23.6 HttpClient

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `connectTimeoutMs` | 10000 | 建立连接的期限。 |
| `readTimeoutMs` | 10000 | 读取头部与流式正文时每次读取的期限。 |
| `callTimeoutMs` | 0 | 整次调用的时限，含等待连接与重定向；`0`表示只受各操作自身时限约束。用尽：抛`TimeoutException`。 |
| `maxRedirects` | 10 | 为`0`时返回`3xx`响应本身而不跟随。 |
| `maxConnsPerHost` | 0 | 同一源站并发连接数的上限；`0`为不限。在`callTimeoutMs`内拿不到连接的调用者得到`TimeoutException`。 |
| `defaultHeaders` | 空 | 该客户端每个请求都加的头——只作空缺补齐：请求已设置过的头以请求为准。 |
| `HttpClient.shared` | 首次使用时创建 | 自由函数使用的客户端（§23.7）。 |

| 成员 | 说明 |
| --- | --- |
| `request(req, callTimeoutMs = 0)` | 缓冲式交换：响应正文整体读入`res.body`。`callTimeoutMs`实参覆盖该次调用的字段值。 |
| `open(req)` | 流式交换：`res.body`保持`null`，`res.bodyReader`即正文（§23.4）。连接归调用方所有，直到正文读尽或调用`close()`。 |
| `close()` | 关闭所有连接池。此后本客户端上的调用抛`HttpException`，消息为`http: client pool closed`。 |

`User-Agent`在请求与`defaultHeaders`都未设置时取`nlang-http/1.1`。请求未带`Host`时由URL填充。

**连接池。** 连接按源站（`host:port`）池化。用完且健康的连接回池复用；出过错误、被告知`Connection: close`、以「读到连接关闭」收尾的正文、或被留下未读正文字节的连接一律关闭而不复用。每源站最多保留四条空闲连接，空闲超过十秒的连接被丢弃——短于服务器自身的30秒keep-alive时限，因此客户端通常在服务器抢先关掉空闲连接之前自己先退役它。借出时的探测只认本客户端自己关过的连接；连接在池中空闲期间被对端关闭不会被发现，借到它的那次请求会以连接错误失败。该请求不会自动重试：失败的连接随即退役，其后一次请求重新建立连接。

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
    cli.maxConnsPerHost = 4;          // 20个请求共用4条池化连接
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

一个客户端、二十个工作者线程、四条连接（§18.11）：每个请求排队等连接，应答从同样几条套接字上回来。`examples/http_conc_bench.n`就是本程序换成一千个工作者。

**重定向。** 带`Location`头的`301`、`302`或`303`响应以`GET`跟随并丢弃正文；`307`与`308`保留方法，并在正文为缓冲时保留正文——流式`bodyChannel`上传无法重放，这类请求收到`307`/`308`时抛`HttpException`而不跟随（§23.4）。`Location`相对发出它的那一跳的URL解析（第22章），故相对目标可用。`Authorization`与`Cookie`不跨越源站边界：改变了scheme、host或端口的重定向不带它们同行。每一跳都计入`maxRedirects`；超过上限抛`HttpException`，消息为`http: too many redirects`。`res.requestUrl`给出返回响应对应的URL。

**gzip。** 在缓冲路径上，若请求未设置`Accept-Encoding`、方法不是`HEAD`且没有`Range`头，客户端加上`Accept-Encoding: gzip`——HEAD响应没有可解压的正文，而`Range`响应是存储字节的切片，自动解压会把它错位。回答为`Content-Encoding: gzip`时，客户端把它解压进`res.body`，并移除`Content-Encoding`与`Content-Length`。你自己设置了`Accept-Encoding`的请求，其响应原样交回：索要某种编码就意味着要自己读那种编码。`open()`既不添加该头也不解压，因为`gzip.decompress`是整缓冲接口（第24章）；gzip的流式正文得手工解压。同一特性的服务器侧是`HttpResponse.compressBody`（§23.5）。

## 23.7 自由函数

| 函数 | 说明 |
| --- | --- |
| `http.get(url, timeoutMs = 0)` | `GET`，响应正文缓冲。 |
| `http.post(url, body, contentType = "text/plain; charset=utf-8", timeoutMs = 0)` | `POST`，`body`按UTF-8编码。 |
| `http.put(url, body, contentType = "text/plain; charset=utf-8", timeoutMs = 0)` | `PUT`，`body`按UTF-8编码。 |
| `http.del(url, timeoutMs = 0)` | `DELETE`——自由函数取名`del`，因为`delete`是关键字。 |
| `http.request(req, timeoutMs = 0)` | 任意请求，缓冲。 |
| `http.defaultClient()` | 其余函数共用的客户端，首次调用时创建。通过它配置（`defaultHeaders`、各超时、`maxRedirects`、`maxConnsPerHost`），或把你自己的客户端赋给`HttpClient.shared`。 |

`url`是`string`，按第22章解析。`timeoutMs > 0`约束该次调用，覆盖共享客户端的`callTimeoutMs`。这些函数一律缓冲：正文很大或要流式时，自己构造请求并调用`HttpClient.open()`（§23.6）。与所有其他客户端入口一样，`4xx`或`5xx`返回响应而非抛异常——只有传输或协议失败才抛。

## 23.8 HttpException

`HttpException`派生自`IOException`（§8.3、§20），是`const class`：一次构造，其字段此后不变。它只有一个字段`status`：

| `status` | 含义 |
| --- | --- |
| `0` | 不是HTTP状态错误——失败发生在状态线之前或之外。 |
| `4xx`、`5xx` | 此次失败用以作答的状态，或从对端畸形消息中读出的状态。 |

异常可以带上`cause`，与其他异常一致（§8.2）。库自己抛出的消息用平实的话说明原因：`http: request url must be absolute`、`http: unsupported scheme https (no TLS in net.n)`、`http: header block exceeds 65536 bytes`（431）、`http: body exceeds 16777216 bytes`（413）、`http: too many redirects`、`http: cannot replay a streaming body across redirect`、`http: client pool closed`、`http: malformed request line`（400）。服务器自己的应答列在§23.5。

对端的`4xx`或`5xx`是响应，不是异常：查`statusCode`。异常意味着交换本身失败了——连接被拒、期限到期、消息无法解析。正因如此，处理器抛出的带状态`HttpException`才能被服务器当作请求级错误来作答；也正因如此，第20章的`SocketException`与`TimeoutException`在本包之下仍保持可区分：它们原样透出。

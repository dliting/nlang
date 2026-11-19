# v0.13.12 WEB框架（stdlib/web多模块包）概要设计

## 一、引言

NLang已经有了HTTP传输与消息层（v0.13.5的`http`包：`HttpServer`每连接一虚拟线程、`HttpBodyReader`流式正文、`HttpClient`连接池与重定向）、URL编解码（v0.13.4的`url`包）、JSON（v0.13.11的`json`包）、正则与压缩。缺的是把「一个HTTP请求变成一次应用响应」的那一层：路由、中间件、会话、静态资源、表单与文件上传。v0.13.12补这一层，形态是标准库源码包`stdlib/web.n`（纯NLang实现，除三枚native外无新增宿主依赖），并顺路清偿`http`库收尾两项（服务器优雅关闭、`net`的EOF判别）。范围已由roadmap钉住（`docs/roadmap.md:124`）。

本版有一个硬前提：不以lambda/闭包为前置（v0.13.10未交付，2026-11-16裁定本版排在其之前）。于是「回调」只有两种现成表达——函数引用与类实例的虚方法。用户已裁定（2026-11-17）：中间件必须同时支持独立中间件函数与子类实现基类虚方法两种风格；multipart/form-data（多部分表单，即表单与文件上传）纳入本版；本版不做的特性一律写进本文第九章备忘，供将来版本实现时参考。

本文是概要设计（Spec）：钉死目标范围、语义裁定与API面；逐任务细化留给实施计划。文风与结构仿`docs/dev/json_library_design.md`。

## 二、目标与范围

### 2.1 本版做

- `web`包（点分多模块包：包级模块`stdlib/web.n`加三枚子模块`stdlib/web/static.n`、`stdlib/web/sse.n`、`stdlib/web/form.n`，预置包自动收编，分层见D-28）：
  - `App`门面与注册面：两种中间件注册、路由注册、前缀挂载、`setNotFound`、`onError`、`useSessions`、`serve`/`listen`/`drain`/`close`；
  - `RequestContext`：每请求的`req`/`res`、路径参数、取值面、cookie、会话、响应快捷方法与`next()`；
  - 中间件洋葱链（全局单链，两种风格同链混排）；
  - 路由：方法＋模式串，`:name`参数段与`*`尾通配，精确表优先，HEAD复用GET，405带`Allow`；
  - 会话：`Session`＋`SessionStore`虚基类＋内置内存store＋cookie安全基线＋登录后`rotate()`；
  - 静态资源：`StaticFiles`（根目录、index回退、按段拒绝目录穿越（解码后段值内的分隔符同判）、点文件默认按不存在处理、目录补尾斜杠301、弱ETag与Last-Modified条件请求、`text/*`带`charset=utf-8`、HEAD）；
  - SSE helper：`Sse`（只做帧序列化与心跳注释，不解释事件语义，见D-16）；
  - 表单与上传：`Form.parse`按Content-Type分派urlencoded与multipart，`FormData`/`FormFile`读取面，构造侧`MultipartBody`供客户端上传；
- `http`收尾：优雅关闭（三态状态机＋连接登记表＋排空/强关＋`start`/`serve`复位＋双启动拒绝）；
- `net`收尾：新增`TcpStream.isEof()`＋补`read`区分超时与对端EOF的对照测试（该项现况已满足，见D-23）；
- 承载native三枚：`math.randomBytes`、`fs.writeBytes`、`fs.modifiedTimeMs`（`net.TcpStream.isEof()`纯源码即可）；另把`fs.readBytes`返回类型由`int[]`改为`byte[]`；
- 示例`examples/web_hello.n`（路由＋两风格中间件＋会话＋静态）与`examples/web_upload.n`（multipart上传往返＋落盘）；
- 手册：新页`library/27-web.md`（中英成对），formats三页27/28/29顺延为28/29/30，`fs`/`math`/`net`/`http`四页与库异常矩阵页同步。

### 2.2 本版不做

每条一行理由，将来实现时的落点与设计要点见第九章：

- WebSocket：需要帧编解码与连接升级，独立成一版更稳（九.1）；
- 模板引擎：`json`与字符串插值已覆盖「数据→文本」主路，模板语法一旦定型改动代价大（九.2）；
- 流式上传落盘（边读边写临时文件）：需要visitor接口＋临时文件生命周期，代价与本版其余部件不匹配（九.3）；
- multipart嵌套部件：RFC 7578禁止表单嵌套multipart，无真实消费者（九.4）；
- 路由级中间件与路由分组：无闭包时表达成本高于收益（九.5）；
- 外置/分布式会话存储与每会话锁：内存store足以承载教学与单进程场景（九.6）；
- 签名或加密cookie（无状态会话）：需要HMAC原语，与本版会话面正交（九.7）；
- CSRF令牌、限流、请求日志、恢复（recovery）等中间件集：属应用策略而非框架内核（九.8）；
- 响应压缩中间件：`gzip`包已有整块`compress`/`decompress`，`http`层也已按`Accept-Encoding`择路并管好`Content-Encoding`、`Vary`与`Content-Length`（D-16末段列实证行），缺的只是「什么响应该压」的决策薄层——按Content-Type与正文大小决定要不要开`res.compressBody`（九.9）；
- 反向代理头信任（`X-Forwarded-Proto`/`For`）：信任边界配置错了比缺更糟，需独立设计（九.10）；
- Range请求（206/416与多段byteranges）：三件套不便宜，静态服务先不承诺断点续传（九.11）；
- 目录列表（autoindex）：与静态服务的缓存语义纠缠（九.12）；
- HTTP/2：分帧层重写，属另一个版本量级（九.13）；
- 表单绑定到struct（把`FormData`灌进字段）：依赖v0.13.9值类型引用与赋值反射面（九.14）；
- 闭包落地后把两风格收敛成闭包写法：v0.13.10之后的替换工作，roadmap已记（九.15）；
- 下载响应头（`Content-Disposition`的生成与`filename*`）：本版只做上传侧解析，响应侧的收件人差异是另一道题（九.16）；
- 会话绝对过期上限与到期滚动刷新：与现有空闲TTL会互相拉扯，须与每会话锁一并裁决（九.17）；
- 部件历史编码解码（`Content-Transfer-Encoding`、RFC 2047、RFC 2231）：RFC 7578已请发送方别再发且无实发实现被发现，一律不透明直通（九.20）；
- 文本部件的charset梯度（部件`charset=`与`_charset_`字段）：现代客户端固定UTF-8，HTML form遗留场景本版无既有用户（九.21）；
- `a[b][c]=`参数树：框架只交保序的(name, value)序列，嵌套结构是应用层语义（九.22）。

### 2.3 版本落点

内容整体入roadmap既有条目**v0.13.12：WEB框架**（`docs/roadmap.md:124`），不新开版本号。multipart本就属「路由/中间件/session/静态资源」的相邻必要件，按用户裁定（2026-11-17）并入本版而非另立条目。第九章备忘项同步补一行进roadmap的「远期特性」清单并指向本文，登记不遗漏。本版不依赖v0.13.9（值类型引用）与v0.13.10（lambda）的任何语言变更。

## 三、关键事实（勘察所得）

### 3.1 承建面：现有标准库给到什么

| 事实 | 出处 | 对本版的意义 |
| --- | --- | --- |
| 服务端请求体一律经`req.bodyReader`交付 | `stdlib/http.n:1445-1446` | 表单与上传正文读取只有一个入口，框架不做二择 |
| `HttpBodyReader.readAll(): byte[]`超`maxBodyBytes`抛`HttpException(413)` | `stdlib/http.n:795-823` | 上传的总体上限已在下游，框架层只需再设更小的表单级上限 |
| 响应流式上行是`res.bodyChannel`（`Channel<byte[]>`），写失败→关通道→`ChannelClosedException` | `stdlib/http.n:174`、v0.13.5交付面 | SSE的背压与取消已在http层，helper只补文法 |
| 处理器契约`func<HttpResponse, HttpRequest>`，异常映射已在http层（`HttpException`→自选状态、其他→500不外泄） | `stdlib/http.n:1449-1459` | `App.handle`沿用同一映射，避免两层语义分叉（D-11） |
| `HttpServer.close()`只置`m_closing`并关监听器，无登记表、无排空；`m_closing`/`m_rejecting`/`m_active`不复位；`start()`覆盖`m_listener`，accept线程只在accept抛错且`m_closing==1`时退出 | `stdlib/http.n:1562-1565`、`:1545-1549`、`:1568-1576` | 优雅关闭与复位项都要新做；双启动会留下永不退出的旧accept线程（D-22实证） |
| 每连接keep-alive循环已有两处服务器回调：循环体`run()`（`:1366-1388`）与收尾的`m_srv.connDone()`（`:1395`）；`HttpConn.close()`是有序关闭（半关＋排空＋最多`CLOSE_GRACE_MS`，`:1292-1296`） | `stdlib/http.n:1361-1396` | 排空检查与登记/注销都有现成接线点，无需新循环；但循环顶部的检查只在连接线程**走到请求边界**时生效：停在`fill`等待里的空闲连接走不到那里（`:1107`→`:909`），因此空闲连接只能由排空一侧从登记表主动关；强关必须另开一口（D-22） |
| 全库的释放动词只有`close()`一个：实测12处公开声明，跨`io`（`ByteStream`/`FileStream`）、`net`（`IcmpSocket`/`TcpListener`/`TcpStream`/`UdpSocket`）、`concurrent`（`Channel`）、`http`（`HttpBodyReader`/`HttpConn`/`HttpServer`/`HttpPool`/`HttpClient`）四包；建立侧动词按对象种类分（`bind`/`accept`/`connect`/`start`），公开面无`terminate`/`kill`/`disconnect` | 12处逐一列出：`stdlib/io.n:108`（`ByteStream`）、`:151`（`FileStream`）；`stdlib/net.n:169`（`IcmpSocket`）、`:248`（`TcpListener`）、`:547`（`TcpStream`）、`:787`（`UdpSocket`）；`stdlib/concurrent.n:67`（`Channel`）；`stdlib/http.n:834`（`HttpBodyReader`）、`:1292`（`HttpConn`）、`:1562`（`HttpServer`）、`:1730`（`HttpPool`）、`:1872`（`HttpClient`） | 服务器关闭沿用同一姿态：释放仍叫`close()`，只给排空另起动词，不让`HttpServer`成为唯一例外；`web`层不必新增打开动词（D-22） |
| `HttpClient.open(req)`的`open`指「发起流式交换」而非「打开连接」：连接由池透明借还，缓冲版叫`request()`；`HttpBodyReader.drain(int)`已占用「排空」一词，义为「吃掉残余正文以便连接复用」 | `stdlib/http.n:1813`（`HttpClient.open`，手册§23.6）、`:842`（`HttpBodyReader.drain`，手册§23.4） | 两个`drain`同包不同receiver，手册用类名限定语区分；`open`是否改名记入九.19 |
| 字节工具：`HttpParser.appendAscii(List<byte>, string)`、`toByteArray(List<byte>)`、`encodeChunk`；`io.toBytes`/`io.fromBytes` | `stdlib/http.n:624`、`:323`、`:313` | 构造multipart正文与解析部件头都有现成工具 |
| `url.percentDecode`/`formDecode`/`percentEncode`/`formEncode`是`url`包文件级函数，可跨模块调用 | `stdlib/url.n:382`（`percentDecode`）、`:453`（`formDecode`）、`:353`（`percentEncode`）、`:421`（`formEncode`）＋探针U1 | 路由参数解码与urlencoded表单复用现成实现，不重复造 |
| `Url.path`保留未解码的原始字节 | `stdlib/url.n:17-36` | 路由必须自行按段解码（D-7） |
| `fs`只有读没有写：`exists`/`isFile`/`isDirectory`/`size`/`listFiles`/`makeDirs`/`remove`/`join`/`readBytes(): int[]`/`readFloats`，无修改时间；文本写只有`io.writeFile`/`appendFile` | `stdlib/fs.n:1-39`、`stdlib/io.n:81/85` | 上传落盘与会话/静态的ETag都缺native（D-15） |
| 熵源只有`math.random()`/`randomi()`/`srand()`，背后是共享`std::mt19937`，种子取自`std::random_device`，且`srand`允许任意代码重播种 | `stdlib/math.n:84-92`、`src/vm/VmExecutor.cpp:71` | 达不到OWASP对会话标识的CSPRNG要求（D-13） |
| 并发设施：`thread.spawn`/`sleep`、`Mutex`、`AtomicInt`、`Channel<T>`、`WaitGroup`；`http`已用`AtomicInt`数活跃连接 | `stdlib/thread.n`、`stdlib/concurrent.n`、`stdlib/http.n:1526` | 会话store加锁与服务器登记表都有现成件 |
| 数组切片是**视图**（别名写回源，O(1)），http层的分块读取正靠它 | `stdlib/http.n:748-749`、`:791-792` | multipart部件可零拷贝切出（D-18） |
| 跨模块虚方法覆写成立 | `tests/e2e/abstract_crossmodule_ok/{lib.n,main.n}` | 中间件基类放`web`包、子类写在用户模块可行 |
| `Dict.keys()`等读取面齐备；`Dict.get`缺键抛异常（无`getOrDefault`） | `stdlib/containers.n:26-36`＋探针D1 | 登记表遍历有现成件；取值面不得裸交`Dict`（D-6） |
| 普通`class`的容器字段就地改动即生效；`const class`的`List`/`Dict`成员读被改写成浅拷贝、改动静默丢失，且其字段不得是数组或非`const`类类型 | 探针C1（`temp/web_probe/ctn/`，普通类两腿计数为2、const腿为0）、手册`04-types/class.md:508-513`与`:515-520` | `web`的可变store与`http`的登记表一律普通`class`（写成`const class`会静默丢数据而不断言失败）；`FormFile`带`byte[]`字段同理不能是`const class`；读写同锁时直改字段即可，无须照搬`stdlib/http.n:23-26`那种「读进本地再写回字段」的姿势 |
| 读方向的EOF已是粘滞标志：`read`返0即对端关闭、超时抛`TimeoutException`；`readLine`对空行与EOF都返空串（源码注释自陈「a string has no null value」）；`readExactly`短读抛`SocketException`；`isClosed()`只看本地已关 | `stdlib/net.n:292-309`、`:455-468`、`:520-546`、`:557`、`src/native/net/net_tcp.cpp:394-400` | 「区分超时与EOF」在`read`上已成立（D-23）；粘滞标志`m_winEof`已在源码里，`isEof()`无需native |
| 客户端连接池已有连接清单＋关闭记账（借出清单`m_active`、`close()`先标记后结算） | `stdlib/http.n:1624`、`:1702-1747` | 服务器登记表的形态有在树先例可循（D-22） |
| 跨线程关闭与parked读的竞争在native侧已经定成「关者胜出」：closer在状态锁内`MarkClosed`、解锁后唤醒等待者（丢唤醒窗口两侧都关死），parked的读以`kNetErrClosed`返回，`net`把它映射成`SocketException("net: socket closed")`；fd的实际关闭推迟到在途操作释放claim之后 | `src/native/net/net_tcp.cpp:9-21`（`:19-21`即「每条退出路径释放claim，最后一次释放才真正关fd」）、`:171-174`、读路径`:252-253`；`src/native/net/net_tcp_park.h:24-29`、`:62-66`；推迟关闭的机制本体在`src/native/net/net_slots.h:5-7`与`net_slots.cpp:93`（`if (slot->closed) Recycle(handle);`）；`src/native/net/net_state.h:37`（`kNetErrClosed`＝-1）；`stdlib/net.n:55`（映射只看错误码、不看调用名，故各条路径文案相同），空闲等待那条路径的抛出点是`stdlib/net.n:424-425`（`fillWindow`，由`stdlib/http.n:985`的`peekFilled`走到）、关闭后的同方向调用由`:300`（`read`）、`:367`（`consume`）与`:381`（`peekWindow`）三处守卫抛同一异常 | 排空期的主动强关不会把连接线程留在无人唤醒的等待里：强关即唤醒，该线程按异常经`run()`的外层catch收尾（D-22因此不必另设唤醒通道，也无需为close与park的先后另立协议） |

### 3.2 语言层面探针结论（决定API形状）

| 探针 | 结论 | 对设计的约束 |
| --- | --- | --- |
| P1 别名可见性 | `using`别名只在单个翻译单元内可见，且不得与被导入声明同名（`src/compiler/builder/DuplicateFieldChecker.hpp:291-341`，手册`03-program-structure/declarations.md`§Type Aliases） | 框架词汇不能以别名跨模块交付，公开词汇必须是类；用户侧写限定名`web.RequestContext`（D-2）。曾试「参数化`Next`」形态，用户签名会退化成`func<void, web.RequestContext, func<void, web.RequestContext>>`，否决 |
| P2 函数引用的绑定规则 | 重载**不**阻挡函数引用：同名候选里只有一个候选的该形参是`func`类型时，裸名实参绑到它并按预期分派（探针X1–X4实测exit 0，覆盖`func`与类两候选、类型别名形参、绑定方法作实参、`mount(prefix, func)`＋`mount(prefix, Router)`四种形状）；真正被拒的是**带默认值的func形参**：`app.use(auth)`报`function reference "auth" requires an expected function type.`（探针F3实测rc=1，闸门在`src/compiler/builder/ExprResolverFuncRef.cpp:85-97`——期望类型必须是`func`实例化，带默认值的形参不走该路径）；同一句诊断还在**实例构造调用的实参位置**复现（`FuncMiddleware(handler)`一类`Type(args)`写法，自由函数引用与绑定方法引用同样被拒，探针`temp/t6_probe3.n`实测2026-11-19；同一枚引用在普通调用实参、字段赋值与带类型的局部声明三处都能绑定，缺口只在构造调用那一格） | 两种中间件风格与「收函数」/「收对象」两种挂载都能以**单名重载**交付（6.2的`use`与`mount`各两候选）；框架公开方法的func形参一律不给默认值；框架内部把方法引用交给构造调用时先落进带类型的局部变量（Task 6的`App`构造`m_routeLink`即此形制），缺口本身登记进九.18 |
| P3 并存形态 | 文件级游离函数可与`main()`并存，同类`static`方法亦可（探针FS1） | 用户示例可直接写自由中间件函数；框架内部工具类可用静态方法 |
| P4 `Dict.get`缺键抛异常 | VM报`NLang VM: Dict key not found`；`containsKey`为`false`（探针D1） | 上下文与会话的取值面带`has`与带默认值的读取，不把缺键异常交给用户（D-6） |
| P5 骨架端到端 | 两模块构建＋真实HTTP往返：子类风格与函数风格混排、短路、后置、异常穿透洋葱、每请求状态隔离四场景全绿（exit 0） | 本设计的中间件形态在现语言上跑得通，无需语言扩展；探针将固化为e2e语料（见七） |
| P6 容器字段改动语义 | 普通`class`在方法内直改`Dict`/`List`字段，跨调用持久（实测计数2）；`const class`同样写法实测计数0——成员读被改写成浅拷贝（探针C1） | 会话store、登记表、`FormData`的部件清单一律用普通`class`；为「防御式」给它们加`const`会静默丢改动而不是编译失败（3.1事实行） |
| P7 容器元素不可就地调用 | 从容器取出的函数引用不能直接调用：`mws[0](ctx)`、`(mws[0])(ctx)`、`mws[0].invoke(ctx)`、`mws[i](ctx)`一律报语法错（探针W1/W2/W4/W6/X6b/P09）；先赋给同类型的局部变量再调用成立（探针W5实测exit 0） | 洋葱链的内部推进必须「取元素进局部」，属实现细节、不改公开面；缺口本身登记进D-27与九.18，不用绕行掩盖 |
| P8 `void`与`func`的容器拼写 | `func<void, RequestContext>`一类作类型实参可编译可运行：`List<func<void, Ctx>>`局部与字段、`Dict<string, func<void, Ctx>>`、返回值装进容器皆实测通过（探针X6a/X8/X9/X14/V5/N03）；同一形状在类型实参拼写缺陷修复前报`corrupt descriptor spelling: unresolved link key '?'`，现编译器已修 | 中间件链与路由表可以直接用容器持函数引用，不必为此专门造类；框架不依赖尚不成立的拼写形状 |

探针脚本在`temp/web_probe/`（含被否决的两版骨架，留作演进证据）。曾出现「游离函数不能与`main()`并存」的连带诊断，经P3探针证伪：那是未解析形参类型的级联报错，不得据以设计。

## 四、调研结论与出处

参考对象：Go`net/http`（`ServeMux`、`fs.go`、`mime/multipart`）、Gorilla`sessions`、express与express-session、serve-static、Flask/Werkzeug、busboy/multer、WHATWG`text/event-stream`、RFC 9110/9112/6265/2046/7578/8187/5987/7230、OWASP Session Management与Session Fixation。逐字取证存本地副本`temp/ref/web_framework_research.md`（六主题）与`temp/ref/multipart_research.md`（multipart，2026-11-17补做），RFC原文存纯文本副本`temp/rfc9110.txt`与`temp/rfc6265.txt`，四者皆不入库，故本表以外部出处自证、文中按行号直引。

| 议题 | 主流做法（出处） | 本框架取向 |
| --- | --- | --- |
| 路由 | Go 1.22起`ServeMux`本身就支持逐段命名参数`{name}`、尾通配`{name...}`、方法前缀与`{$}`精确子树，优先级定为「静态＞命名参数＞catch-all」，同层冲突在**注册期**直接`panic`；express系按注册序覆盖，是其公认痛点（调研注§1） | 取Go 1.22的语法子集（`:name`代`{name}`、`*`代`{name...}`），并采纳它的「静态优先」优先级与注册期冲突拒绝（D-7/D-9）；不在路由表内引正则（已有`regex`包，需要时用户自匹配） |
| 中间件 | Go：`func(http.Handler) http.Handler`（依赖闭包）；express/Koa：`(req,res,next)`洋葱，`next()`推进、Koa以`await`回程；ASGI/ASP.NET Core：管道装配 | 无闭包→「上下文自带`next()`」替代`func(req,res,next)`；错误不走`next(err)`而走异常穿透（D-3） |
| 会话存放 | express-session：「Session data is not saved in the cookie itself, just the session ID」，数据在服务端；默认MemoryStore自陈「not designed for a production environment. It will leak memory」 | 同形态；内存store的单进程定位写进手册而非隐藏（D-12） |
| 会话标识强度 | OWASP：标识符必须由CSPRNG生成、熵≥128位；express-session要求secret≥32字节熵；Session Fixation要求权限跃迁后换新ID | 新增CSPRNG native，id=16字节→32位hex，登录后`rotate()`（D-12/D-13） |
| cookie属性 | MDN`Set-Cookie`：`HttpOnly`、`Secure`、`SameSite`、`Path`、`Max-Age`语义；`__Host-`前缀要求`Secure`＋`Path=/`＋无`Domain` | 会话cookie默认`HttpOnly`＋`Path=/`＋`SameSite=Lax`，`Secure`与`__Host-`按配置（D-12） |
| 未初始化会话 | express-session `saveUninitialized=false`省存储并规避并发首请求竞态 | `ctx.session()`被取用且有写入才发`Set-Cookie`（D-12） |
| 静态服务 | Go`FileServer`：index回退、`..`清洗、按扩展名定Content-Type、`If-None-Match`→304、`If-Modified-Since`秒粒度、Range（206与多段）；但**默认不生成ETag**——`serveContent`只调`setLastModified`（`temp/ref/src/go_fs_source.go:278`，全文无写ETag的语句），条件校验在`checkIfNoneMatch`与`checkPreconditions`里、只拿响应里已设的ETag做弱比较（:557、:658），`writeNotModified`到304只做RFC 7232 §4.1的响应头清理（有ETag才删`Last-Modified`，:640-641），ETag归调用方自设。自动生成ETag的一方各有格式风险：调研注§4坑2记录公式含inode或随机因子时每次重启缓存全废，而nginx`etag on`（1.3.3+）虽自动生成，具体格式却未取证（注:225/:397自认，且明令勿引mtime-size细节）。serve-static默认`maxAge=0`**仍**发`Cache-Control`（`cacheControl=true`）；「不发定时max-age、靠条件请求保鲜」是Flask`max_age=None`那一档的姿态 | 采纳其子集；弱ETag的逐字节格式`W/"<size>-<mtimeMs>"`是本框架自定（调研注只背书「取稳定字段」这一取向，没给格式），Range与目录列表递延（D-14、九.11/12） |
| SSE | WHATWG`text/event-stream`：`Cache-Control: no-cache`、帧由`data:`行加空行终结、`:`开头为注释、`id:`/`event:`/`retry:`可选；反代缓冲与压缩是**调用侧义务**而非文法条款——WHATWG的示例头带`X-Accel-Buffering: no`；nginx`proxy_buffering`默认开、`X-Accel-Buffering: no`可关（调研注§5逐字取证），fastcgi栈的响应缓冲同属nginx侧默认行为、gzip攒块属压缩通识，这两条注内无逐字出处 | 只做分帧、心跳注释与反代友好头（`X-Accel-Buffering: no`）；「逐帧flush」不需新机制、事件流也不会被压缩，两者的机制根据见D-16，缓冲响应「要不要压」的策略归九.9（D-16，roadmap原文） |
| 优雅关闭 | Go`Server.Shutdown(ctx)`：关监听→盖`Connection: close`→**主动关闭idle连接**→等在途请求完成→ctx截止即返回；另有`Close()`强关；nginx`QUIT`：不再接受新连接，worker处理完在途请求再退出，另有`worker_shutdown_timeout`（1.11.11+，默认不设）到点强制关剩余连接；「排空时限必须小于进程退出总期限」这层关系出自Envoy（`--drain-time-s`默认600s须小于`--parent-shutdown-time-s`默认900s，调研注§6） | 三态状态机＋登记表＋`drain(graceMs)`排空＋`close()`强关，并补齐Go`Shutdown`常被漏掉的两半：其后响应一律盖`Connection: close`、停在keep-alive的空闲连接主动关掉（D-22） |
| multipart文法 | RFC 2046 §5.1：boundary为1..70字符（不计两个前导连字符）的bchars集、preamble与epilogue「implementations must ignore」、定界行必须居行首且「the initial CRLF is considered to be attached to the boundary delimiter line」、匹配语义「An exact match of the entire candidate line is not required」、含tspecial的boundary参数值必须加引号；RFC 7578：§4.2部件必须带`Content-Disposition`且含`name`、有`filename`参数即文件（可缺可空）、§4.3不嵌套、§4.4部件缺省Content-Type为`text/plain`、§4.7已废Content-Transfer-Encoding、§4.8其余部件头必须忽略、§5.2同名部件不得合并、§4.2 NOTE「The encoding method described in [RFC5987] … MUST NOT be used」；`filename*`双写属**响应侧**下载头（RFC 6266 §4.3「recipients SHOULD pick "filename*"」，MDN「the request header does not have the filename* parameter」） | 上传解析按RFC与主流宽容度收齐，不理解请求侧`filename*`；优先级只在生成下载响应头时适用（D-19、九.16） |
| multipart限额 | Go：`NextPart`/`ReadPart`限**每部件**头部数10000，`ReadForm`另限部件数1000与全部文件部件头部数**合计**10000（`GODEBUG=multipartmaxparts`/`multipartmaxheaders`可调），`defaultMaxMemory`＝32 MiB另加10 MB字段预算，超限文件spill进共享临时文件，报`ErrMessageTooLarge`；Werkzeug/Flask：`MAX_FORM_MEMORY_SIZE` 500 KB、`MAX_FORM_PARTS` 1000、500 KB spool；Django：非文件请求体2.5 MB、字段1000、文件100、单文件2.5 MB转存磁盘；busboy：`fieldSize` 1 MiB、`fieldNameSize` 100、`fields`/`files`/`parts`均Infinity、部件头只有硬编码的16 KiB与2000对（node值），超限报「Malformed part header」；Rack：文件部件128、总部件4096、驻留16 MiB、parser总量10 GiB；nginx`client_max_body_size`缺省1 MiB→413；PHP：`post_max_size` 8 M／`upload_max_filesize` 2 M／`max_file_uploads` 20，超限置空`$_POST`不报错 | 内存内解析＋三道可调上限（整段body/部件数/单部件）越限显式413；另两道**文法级**上限（部件头16 KiB、字段名100字节）报400并具名，与D-19的拒绝面同口径；busboy的截断置位与PHP的置空不报错都是把超限变成隐式状态，不取；流式落盘递延（D-18） |
| 上传文件名 | 解析侧义务：RFC 2183 §2.3「The filename should be treated as a terminal component only」＋RFC 7578 §4.2「do not use the file name blindly … do not use directory path information」；Rack剥路径时`split(/[\/\\]/)`同时覆盖两个分隔符（IE全路径与HTML`C:\fakepath\`遗留），Go`filepath.Base`按宿主平台判分隔符是半修法；文件名默认是原始字节，擅自percent解码会篡改真名（HTML构造侧「must not perform any other escapes」）；存储侧：Werkzeug`secure_filename`（NFKD转ASCII、Windows设备名加`_`前缀）、Django`get_valid_filename`（更弱、不处理设备名）、Rails ActiveStorage`sanitized`（危险字符换`-`）、OWASP《File Upload》建议应用自生成名＋长度上限＋扩展名白名单 | `filename`按RFC剥路径后原样供回显＋`safeName()`清洗＋`saveTo(dir)`加随机后缀落盘（D-20） |

## 五、架构裁定

每条给裁定、独立理由与代价。「现状如此」只作事实引用，不单独充当理由。

### D-1 分层：组件库为核＋`App`薄门面

`web`包内`Router`、`SessionStore`、`StaticFiles`、`Sse`、`Form`各自独立可用（各归哪一枚模块见D-28）；`App`只做注册与分派：中间件链→路由表（挂载条目按D-7的「静态精确表优先、其后注册序」参与同一张表）→整表未命中才轮到notFound/405兜底（D-10）。
理由：每块能单独测试与替换，`App`不随功能膨胀；静态服务或会话不经中间件也能单用。代价：`App`的转发方法略重复。

### D-2 公开词汇一律用类，不用跨模块别名

框架暴露的类型都是类；用户在自己的模块里写限定名（`web.RequestContext`），或换用不与导入名冲突的别名（`using Ctx = web.RequestContext;`）。
理由：别名TU内可见、且不得遮蔽导入声明（P1），是语言既定语义；把词汇表押在别名上会让跨模块用户签名不可读。代价：用户签名略长。

### D-3 中间件＝以`RequestContext.next()`推进的洋葱链，两种风格同链

```
// 函数风格：注册时直接给函数名
void auth(web.RequestContext ctx) {
    if (ctx.req.headers.has("X-Key") == false) { ctx.text(401, "missing key"); return; }
    ctx.next();
}
// 子类风格：覆写process，可持有配置与状态
class Logging : web.Middleware {
    public void process(web.RequestContext ctx) {
        ctx.next();
        ctx.res.headers.set("X-Status", ctx.res.statusCode.toString());
    }
}
```

`Middleware.process`的基类默认实现就是`ctx.next()`，故「不覆写＝透传」；函数风格经包内适配器接入同一条链。短路＝不调`next()`直接返回；回程＝写在`next()`之后；异常直接`throw`，沿洋葱外抛而不被吞。
理由：两风格并用是用户裁定（2026-11-17）；`ctx.next()`是唯一能让两风格签名同形的形态——参数化`Next`形如`func<void, RequestContext, Next>`，而`Next`跨不了翻译单元（P1），用户会被迫写全嵌套函数类型。洋葱模型（express/Koa/ASGI共识）比线性管道多出回程阶段，且天然支持短路。代价：漏调`next()`即终结该层，靠示例与手册钉死，且基类默认实现给出安全缺省。

### D-4 `next()`重入保护

同一上下文第二次推进抛`HttpException("web: next() called more than once", 500)`。
理由：Koa社区同类缺陷高频（其诊断原文即`next() called multiple times`），静默重复执行下游会造成重复副作用（重复扣款、重复写库）；框架自身的编程错误应当外泄消息而非藏进通用500。代价：多一个状态标志。

### D-5 本版中间件为全局链

不做按路由挂中间件。
理由：无闭包时按路由包装中间件要为每条路由生成一组注册重载，表达成本高于收益；`mount`已能给出前缀级的组合点。代价：路径级差异化需在handler内自判（九.5）。

### D-6 `RequestContext`＝每请求状态容器

公开面见6.3。取值面为`has`/`get(key, def)`/`getString`/`getInt`/`getBool`/`set`/`remove`，缺键一律返默认值而不抛。
理由：`Dict.get`缺键抛异常（P4），把裸`Dict`交给用户等于把控制流押在异常上；带默认值的读取是唯一「读不到就有合理缺省」的形态，也是探针踩过的坑（骨架首轮五条失败中三条源于缺键异常）。每请求新建实例（探针P5场景4钉死隔离性）。代价：无类型化容器（要放自定义对象须经`Object`与`as`转型）。

### D-7 路由匹配：逐段、`:name`、`*`，精确表优先

模式串按'/'切段，段数相等才匹配；`:name`捕获一段，`*`必须居末并捕获余下（含'/'）；同方法下先查静态段精确表，再按注册序扫模式，**挂载条目以前缀参与同一张注册序表**（D-10），于是「哪个请求由哪条登记服务」在注册完就完全确定；匹配用原始路径字节分段，捕获值再`url.percentDecode`，解码后若含'/'原样保留。
理由：「先静态精确表、其后按注册序扫」与Go 1.22`ServeMux`的「静态＞命名参数＞catch-all」优先级同向（调研注§1），行为可预测、无需trie构建；正则留在`regex`包里由用户自取，避免路由表隐式绑定引擎。代价：无正则路由与约束式模式（`{id:[0-9]+}`），需要时在handler里自查。

### D-8 方法匹配与405/404

HEAD请求复用GET路由（http层已实现HEAD只发头，`stdlib/http.n:1432`），显式注册的`head`优先；路径命中而方法不集→405并带`Allow`；无命中→404。
理由：HEAD必须可发到GET所指向的资源是HTTP语义要求（RFC 9110 §9.3.2）；405不带`Allow`不合规（§15.5.6）。代价：HEAD的`Allow`串需合并GET登记。

### D-9 注册期冲突即失败

以下情形一律抛普通`Exception`，消息具名冲突双方与首个注册处，启动即终止：同方法同模式重复注册；**同层遮蔽**——同一方法集内同一段位出现两个参数段或参数段与`*`（`/x/:a`与`/x/:b`、`/x/:a`与`/x/*`）、同层两个`*`catch-all；**死条目**——后登记的**含参数段或`*`的**条目被同方法集内某条先登记条目吃掉它的全部候选路径，判据取两条结构关系之一：先登记条目是`mount`前缀（挂载条目自身即算）且该前缀是后登记条目静态前缀的前缀或与之相等；先登记条目是`*`catch-all且其静态前缀是后登记条目静态前缀的前缀或与之相等。此处「静态前缀」指首个参数段或`*`之前的静态段序列（`/static/*`的是`/static`，`/*`的是根），路由与`mount`条目在判据两端同等对待；全静态模式不受此判据约束（D-7的精确表优先已保证它可达）。
理由：路由冲突是程序员的错，静默后者覆盖会产生「线上路由与代码不符」且极难复现；调研注§3点名的正是这类静默遮蔽（`/users/{id}`先注册会吞掉后注册的`/users/new`），Go因此把同层冲突改成注册期`panic`（§1）——先登记的条目吃掉后登记的条目而无任何提示，是主流公认的痛点而非可接受行为。判据按可达性写而不按「字符串是否包含」写，相等边界因此无需另立规则：`mount("/api", X)`在先时后登记的`get("/api", h)`可达（精确表先查，`/api`精确命中不进前缀扫描）故放行——express式`/api`＋`/api`子树的网关写法合法；后登记的`get("/api/:id", h)`的每条候选路径都落在`/api`前缀之下故拒绝——写出来就是一条永不生效的路由。同一判据也覆盖`mount("/api")`后再`mount("/api/v2")`（后者整条死在前者之下）与`any("/*")`后再`mount("/api")`（兜底吃掉后登记的一切参数化条目）。参数段只匹配同段数路径、不吃掉更深的路径，因此不进判据。代价：判定只看同段位与前缀这类结构关系，不做「两条模式是否可能交叠」的完备判定（`/a/:id`与`/a/b`同层并存仍放行，因为静态表优先已保证后者可达）；`any("/*")`一类兜底路由因此只能作全表末位登记，想在所有路由之前跑逻辑该用中间件（D-5）而非兜底路由，根挂载`mount("/", …)`同理是判据下最宽的条目（D-10）；动态注册场景（运行期改表）改由D-25的只读规则约束。

### D-10 挂载：`mount(prefix, handler)`剥前缀

按前缀命中后从`ctx.path`剥去该前缀，handler看到的是余下路径；请求路径恰等于前缀时余下`"/"`（与express`app.use`同调，子树根处理器因此可以按`/`登记）。前缀允许`"/"`——它是'/'起始与'/'结尾两条规则的唯一例外，因为余下路径必须以'/'起始（剥掉唯一的'/'就只剩`api/x`这种不以'/'起始的残串），故根挂载不剥任何字节、余下即请求路径原样；它是全表最宽的前缀条目，凡在其后登记的同方法集参数化条目都被D-9拒在注册期，因此根挂载的典型位置是**最后**登记（`app.mount("/", assets.serve)`＝API路由之后的静态兜底），要把它放在前就得接受后续参数化条目让位。分派按D-7的「静态精确表优先、其后注册序」，挂载条目作为**前缀条目**参与同一张注册序表；一旦某条挂载前缀命中，该子树即归它所有——handler返回即本轮分派结束，**不回落**到后面的条目，因而也不需要还原`ctx.path`。前缀命中而子树内无资源由被挂载组件自己写404（方法不匹配写405带子树`Allow`），前缀不命中则一律不改`res`；404的最终兜底只在整张表（含挂载条目）都没命中时由`App.handle`给（D-8/D-11）。
挂载实参两种形态：`func<void, RequestContext>`与`Router`——后者是`Router.serve`的糖（`app.mount("/api", api)`），子路由是「分组」这一常见形态的主体；其余组件仍走绑定方法（`app.mount("/static", assets.serve)`，D-3的绑定方法作函数引用已实测）。两个候选里只有一个是函数类型，函数引用照旧绑到它（探针X4实测exit 0，见3.2的P2行）。
理由：「前缀命中即让渡子树」把分派变成注册期就完全确定的映射，用户读注册序就能预测谁服务，也不引入「handler到底写没写`res`」这种要靠运行时标志或响应快照才能判定的二义（`ctx.res`是公开字段，用户可直接写，框架无法观测）；一个机制两处用，避免`StaticFiles`自持mount字段（两处字段会漂移）；挂载面只有一种函数类型是无闭包语言下唯一能同时喂给类方法与自由函数的形态，`Router`重载只是给最常见的分组写法省一次`serve`。代价：挂载组件必须自己交代未命中的路径（写404或405），顶层的`setNotFound`与`onError`只覆盖全表未命中的情形，子树内的404文案统一要等九.5的路由分组；一个前缀只能有一个主人，后登记的`app.post("/api/upload", h)`整条落在先登记的`app.mount("/api", other)`之下会被D-9拒在注册期（调换注册序或换前缀即可共存）；handler自己不写响应就是空响应（与顶层handler同姿态）。

### D-11 错误映射沿用http层

`App.handle`catch `HttpException`→用其`status`＋消息进正文（与`serveOne`同形）；catch 其他`Exception`→500＋固定文案不外泄；`onError(func<void, RequestContext, Exception>)`可覆写500的呈现；404走`setNotFound`（默认纯文本）。
理由：web层与http层若各有一套异常语义，用户就得记两套；沿用现成映射也保住「不泄内部细节」的既有姿态。代价：框架级编程错误的消息会进正文（D-4的可诊断性优先，属自觉取舍）。

### D-12 会话：`Session`＋`SessionStore`虚基类＋内存store＋cookie基线

会话数据在服务端，cookie只放id；id=16字节CSPRNG→32位hex；cookie名`nlang_session`，默认`HttpOnly`＋`Path=/`＋`SameSite=Lax`，`Secure`与`__Host-`前缀可配；`maxAgeSeconds`→`Max-Age`的映射全框架统一三态——0不发该属性（会话级cookie）、正数发`Max-Age: n`、负数发`Max-Age: 0`（立即过期）：0落在「不发」一侧与`StaticFiles.maxAgeSeconds`的「0＝不发`Cache-Control`」同调（同一个数在同一框架内扛两种含义必漂），默认值因此给会话级这个最不意外的结果；删除走负数并把线上字节直接写成`Max-Age: 0`，不依赖收件人对负数的处置（`Max-Age ≤ 0`的判定本就是立即过期：调研注§3.2引MDN如此；RFC 6265 §5.2.2对`delta-seconds ≤ 0`是把过期时刻**直接置为「最早可表示日期时间」**（`temp/rfc6265.txt:1091-1092`），按§5.3「过期日期在过去即已过期」的定义（:1318）它必然已在过去、并被「MUST逐出过期cookie」清掉（:1320-1321），而不是钳到0再按「当前时刻＋0」算成「过期时刻＝提取时刻」把临界判定留给收件人的比较符；线上字节写死0仍免去这份实现差异），`Session.destroy()`正是用负数删掉本次cookie；会话cookie本身不带`Max-Age`，过期判定在服务端——客户端时钟不该决定会话寿命，而绝对上限与到期滚动刷新要同`expiresAtMs`一起裁决，故不在此单发一个`Max-Age`（九.17）；`ctx.session()`首次取用即建，**有写入才发`Set-Cookie`**；权限跃迁（登录）必须`rotate()`换新id；过期判定按空闲TTL（绝对上限与到期滚动刷新递延，见九.17），比较基准是`system.monoMs`；清扫由store的`int sweepExpired(long nowMs)`承载：store不自起后台清扫线程，而是按`sweepIntervalMs`在`load`/`save`时顺带触发（惰性清扫），测试也可直接调它（确定性可测）。`Set-Cookie`由`App.handle`在链条跑完、返回响应之前写回`res`：`writeResponse`要等处理器返回之后才被调用（`stdlib/http.n:1469`）、响应头要到`writeHead`才序列化（`:1252`），所以连流式（SSE）处理器里新建的会话也来得及带上cookie。
理由：express-session的三条教训（id熵、MemoryStore泄漏、未初始化会话白写）各自对症；虚基类而非「传个Map进来」是为把「换Redis」留在用户侧而不改框架；清扫入口带`nowMs`参数是为了不过拟合真实时间（既有裁定：断言不得依赖时间）；不起后台清扫线程是为了少一个生命周期——清扫点与读写同在store的锁内，`main()`返回时也不会留下一条正睡在间隔里的线程要交代。代价：单进程内存态，手册照express的姿态明写；且长期无读写的store里，过期记录要等下一次`load`/`save`才被清掉（惰性清扫的固有代价，长驻服务不受影响）；`maxAgeSeconds`的三态是读手册才知道的约定（默认0删不掉cookie），手册要把三态与「`destroy()`用负数」一并写全。

### D-13 熵源：新增`math.randomBytes`

`native byte[] math.randomBytes(int count)`，Windows走`BCryptGenRandom`、POSIX走`getrandom()`（不可用则`/dev/urandom`），不经`VmExecutor`的mt19937、不受`srand`影响。会话id与multipart boundary必须用它。
理由：OWASP把CSPRNG列为硬要求，而`srand`允许任意代码重播种、MSVC的`std::random_device`并非密码学质量源；把「安全随机」与「统计随机」混在一个源上会让前者被后者污染。代价：新增native一枚。

### D-14 静态资源：Go`FileServer`的行为子集

`root`构造、经`mount`挂前缀；请求段percent解码后**按段**清洗——任一段等于`..`或`.`、含NUL、以'/'或盘符起始或根化、解码后的段值内含'/'或'\'、或以`.`起始而`allowDotFiles`为假→404（一律拒绝而非折叠，不靠`stat`试探）；目标为目录且请求路径未带尾斜杠→301且`Location`＝请求路径＋'/'（query原样保留）；目录→`index`（默认`index.html`，无则404）；Content-Type按扩展名小表（HTML/CSS/JS/JSON/常见图片与字体/存档约20项）＋`addType(ext, mime)`可扩，表内`text/*`一律带`; charset=utf-8`，未知→`application/octet-stream`；弱ETag`W/"<size>-<mtimeMs>"`＋`Last-Modified`＋`If-None-Match`/`If-Modified-Since`（秒粒度）→304；`maxAgeSeconds > 0`才发`Cache-Control: public, max-age=`；POST等非GET/HEAD方法→405并带`Allow: GET, HEAD`（RFC 9110 §15.5.6把405响应的`Allow`定为MUST，与D-8「405不带`Allow`不合规」同一判据）。
理由：按段拒绝把「穿越」变成结构判定，字符串折叠＋`stat`的写法历史上反复出穿越CVE；段内编码分隔符必须列进清单，因为D-7是**先按裸'/'切段、再逐段解码**，切段这一步不保证段内没有分隔符——`a%2F..%2Fb`解码后是单个段值`a/../b`，四条老判据全部通过，于是`..`被交给文件系统折叠、读取越过服务根（Windows形态`%5C`同理），这正是本判据要根除的那一类；尾斜杠301是四方共识（Go`fs.Server`、serve-static、httprouter、Werkzeug都重定向，调研注§4坑8），缺它目录页里的相对资源URL全部指向错处而404；`text/*`带charset同样是调研注§4坑9收录的历史XSS面（无charset时浏览器嗅探），且框架自家`ctx.text`/`ctx.html`本就带charset，只有静态面不带会是同一框架内的两种姿态；点文件默认按不存在处理（404）取serve-static`dotfiles: 'ignore'`的缺省，泄露与否交给`allowDotFiles`一个开关而不是由实现者习性决定。ETag带`W/`前缀取弱式，根据是RFC 9110 §8.8.3.3注：内容编码是表示数据的属性，强ETag必须在gzip与identity两个变体之间不同，弱式没有这条约束；而`If-None-Match`按§13.1.2一律用弱比较函数，故两个变体共用一个弱ETag仍能命中304，九.9那枚压缩中间件日后接到静态面不必换ETag。代价：不做Range与目录列表（九.11/12）；`allowDotFiles`只有开与关两态，serve-static那三态里`'deny'`（403）与`'allow'`之外的一态在本框架没有对应物——挂载子树归`StaticFiles`所有（D-10），没有「让给后面的条目」这回事，故'ignore'落成本方法的404；`'deny'`要区分「存在但被拒」与「不存在」，为省一个状态码不值得多开一口。

### D-15 `fs`面三件变更

新增`native void writeBytes(string path, byte[] data)`（截断写）；新增`native long modifiedTimeMs(string path)`（不存在抛`IOException`）；`readBytes`返回类型`int[]`→`byte[]`。
理由：上传落盘必须有二进制写（现只有`io.writeFile`文本写，二进制会坏字节）；ETag与`Last-Modified`必须有mtime；`byte[]`已是http正文与multipart部件的表示，`int[]`与之并存会在每处边界补转换（v0.12.7登记「fs二进制读取对」时`byte[]`面尚未成熟，现以`byte[]`统一）。代价：`readBytes`返回类型变更，在树消费者仅`examples/mnist_mlp/data.n`与`tests/e2e/fs_read_bytes_floats.n`两处，随本版一并改（不向下兼容）。

### D-16 SSE helper只做分帧

`Sse(HttpResponse res)`置`Content-Type: text/event-stream`＋`Cache-Control: no-cache`＋`Connection: keep-alive`＋`X-Accel-Buffering: no`并接`res.bodyChannel`；每帧走一次通道写即一个分块，泵每收到一个通道元素就立刻写出、不等攒块（分块写分支`stdlib/http.n:1261-1262`调用`pumpChunked`，其循环体`:1340-1343`是一元素一次`writeAll`）；`send`三种元数（`data`／`event`,`data`／`id`,`event`,`data`）、`comment`、`close`；帧序列化按WHATWG（`data:`按行拆、空行终结、`\r\n`行尾）。构造时新建有界通道挂到`res.bodyChannel`，典型用法是处理器内`thread.spawn`生产者后立刻返回——响应头要等处理器返回才写出、泵才开始抽通道（`stdlib/http.n:1252`才序列化响应头、`:1261`才把它写出、`:1262`起泵），在树`examples/http_sse.n`即此形态；`send`因背压阻塞，客户端断开时泵侧关通道、`send`抛`ChannelClosedException`，生产者照此退出。
理由：roadmap原文限定「本项目只给HTTP分帧能力不解释SSE语义」；有界`Channel<byte[]>`的背压与取消已在http层，helper只欠文法与保活。缓冲相关的两件事是**调用侧义务**而非协议保证：反代默认攒响应体，缺`X-Accel-Buffering: no`时nginx会把事件流攒到超时或缓冲满才吐（调研注§5 pit1，WHATWG自己的示例也带这个头），而「逐帧flush」在本框架里不需要新机制：`pumpChunked`每个通道元素一次`writeAll`（`stdlib/http.n:1340-1343`），`writeAll`只把一次写切成`WRITE_SLICE`＝64 KiB的分片、不跨元素攒（`:206`、`:1043-1052`），读侧的8 KiB窗口`READALL_WINDOW`（`:679`）与头体拼接上限`JOIN_CAP`（`:865`）都不在这条路上——攒块只会发生在客户端`readAll`那一侧。压缩也无需「跳过」：`writeResponse`的gzip分支门槛是`!noBody && res.compressBody && body != null && acceptsGzip(req)`（`:1209`），`Sse`既不开`res.compressBody`也只填`res.bodyChannel`而不填`res.body`，两条判据都不成立，事件流因此不带`Content-Encoding`（把两者都填又开`compressBody`的混用形体会给通道响应盖上`Content-Encoding`而字节仍按通道原样泵出，那不在本版面内，归九.9那条「见到`bodyChannel`就不碰」一并处理）；线上那半（`Accept-Encoding`择路、`Content-Encoding`、`Vary: Accept-Encoding`、压完才写`Content-Length`）`http`层已经做完（`:1210-1212`、`:1230-1231`），九.9要补的只是「什么响应该开`res.compressBody`」这条策略半，与本节的流式路径无涉。代价：不做客户端`Last-Event-ID`重连语义，由用户自理。同一方法名给多个元数是安全的：候选唯一性只在函数引用实参上起作用，而`send`的三个元数既不带func形参也不带默认值（P2）。

### D-17 表单入口`Form.parse(ctx)`按Content-Type分派

`application/x-www-form-urlencoded`→按'&'与'='切分，键值各走`url.formDecode`/`url.percentDecode`（'+'→空格）；`multipart/form-data`→D-18/D-19解析器；其余Content-Type→`HttpException(415)`；无正文（GET或空body）→空`FormData`而非异常。
分派前先过一次media type参数解析（类型名大小写不敏感、参数表支持quoted-string去引号）：`boundary`的取值要靠它，`application/x-www-form-urlencoded; charset=utf-8`这类带参数的类型也不得被判成「其余」。标准库现状没有这枚解析器（`stdlib/http.n`只写Content-Type、从不读参数，`stdlib/url.n`也无相应函数），故本版在`web.n`内实现一枚私有helper，不新增公开面；响应侧（内容协商、下载头）将来要用时再提升到`http`面（九.23）。
理由：一个入口两形态，避免用户先自判Content-Type（易漏，且漏了就会把带参数的类型读成不支持）；415是RFC 9110 §15.5.16`Unsupported Media Type`的准确语义（「the content is in a format not supported」），而缺boundary、缺`Content-Disposition`这类是客户端语法错，走400（RFC 9110 §15.5.1），把语法错报成415会诱导客户端换个格式重试而白跑一趟；无正文取空集是「读不到就有合理缺省」的一致姿态（D-6同调）。代价：要收`text/plain`等自定义正文的用户须直接读`bodyReader`。

### D-18 multipart解析：内存内单趟、行首锚定、三道上限

读`ctx.req.bodyReader.readAll()`（整体超体的413由http层给出），再扫描定界行：定界行只能出现在正文开头或某处换行之后（RFC 2046 §5.1「The boundary delimiter MUST occur at the beginning of a line」），匹配判据取Go`matchAfterPrefix`的形态——行首两个连字符＋boundary＋其后只能是空格/制表/换行/`--`/正文结尾，故正文行`--foobar`不会被当作`--foo`的定界行、行中出现的相似字节也不会误配（同节NOTE TO IMPLEMENTORS「it is sufficient that the boundary appear in its entirety following the CRLF」）；总比较代价线性，部件正文以切片视图零拷贝切出。三道上限集中在一个`FormOptions`里（`maxBytes`默认`DEFAULT_MAX_FORM_BYTES`＝8 MiB、`maxParts`默认`DEFAULT_MAX_FORM_PARTS`＝1000、`maxPartBytes`默认`DEFAULT_MAX_FORM_PART_BYTES`＝4 MiB），作`Form.parse`的可选第二参数；越限抛`HttpException(413)`且消息具名撞上哪一道。上限装进一个配置对象而不是三个带默认值的形参：三个形参里只想改一个，就得把其余两个重抄一遍。
缺省值取法（各主流缺省值见第四节）：8 MiB对齐PHP`post_max_size`的缺省、且低于http层`maxBodyBytes`的16 MiB，使表单超限先由web层给出具名消息（两层都设时有效上限是二者较小值，手册要明说这层关系）；部件数1000同时是Go`GODEBUG=multipartmaxparts`的缺省与Flask`MAX_FORM_PARTS`，比Rack的4096紧一档、比Django的「文件100＋字段1000」宽松而齐平；单部件4 MiB防「一个部件吃满整段预算」，比整段上限再紧一档（busboy`fieldSize` 1 MiB与Werkzeug字段驻留500 KB是同一维度的收紧）。字段与文件共用`maxPartBytes`：内存内模型里两者成本同为字节，「字段小而文件大」的策略差由应用在`saveTo`前比一次`data.length()`表达，不为此多开一道上限。部件头与字段名各有D-19的一道**文法级**上限（16 KiB与100字节，越限400），故`FormOptions`里只放「可调资源预算」这一类；不另设「头对数」上限，字节上限已覆盖头对数维度（busboy的`headerPairs` 2000对与node的16 KiB本就是同一维度）。字段名那道100字节是调研注§7的采纳项（busboy`fieldNameSize`与multer同为100），本框架采纳：整段body上限早已把名字总字节封顶（≤`maxBytes`），这道独立短限的价值不在内存而在**具名诊断与下游防御**——字段名常直接进日志、数据库列名和`safeName()`之后的文件名，畸形客户端拿到的是`field name exceeds 100 bytes`而不是含糊的「部件头过长」。
理由：http层已有16 MiB的`maxBodyBytes`兜底，框架层再设一道更贴近表单实际量级的上限，比「收得下就全解析」稳；内存内路线在现语言上代价最低且部件零拷贝（3.1的视图事实），而「≤16 MB整体在内存」正是各主流接受的工作点（nginx缺省1 MiB、Django非文件请求体2.5 MB、Rack驻留预算16 MiB、Go`defaultMaxMemory` 32 MiB都在其内或略高）；流式路线要为visitor接口＋临时文件＋清理另花一份功夫（Go为此带`bufio.Reader`与spill逻辑），且在body已在手的前提下只是把同一批字节搬两次，本版不做（九.3）。整体在手还有个错误语义红利：缺闭合线这类文法错可以立刻判400，不必像Go流式那样在「数据还没到」与「文法坏了」之间含糊。代价：整段正文驻留至最后一个部件引用释放，手册标注；超出`maxBytes`的上传收不下（本版姿态：调高上限，或等九.3的流式落地）。

### D-19 multipart文法逐条（RFC 2046 §5.1、RFC 7578、RFC 7230 §3.2.4）

- `boundary`参数必须存在且非空，缺失→`HttpException(400)`；参数值按D-17的media type参数解析取（含tspecial的边界必须加引号是RFC 2046 §5.1自己的WARNING，取值后去引号），两端与行尾水平空白剥掉（同节：疑似网关添加的空白「must be deleted」）；出现多于一个`boundary`参数→400，不做「取最后一个」的猜测（Rack同款拒绝）；收件侧不因长度或字符集主动拒绝——1..70与bchars是构造侧义务（Go也只在`SetBoundary`生成侧校验），收件时长度已由部件头上限兜住；
- 首个定界行之前的preamble容忍并忽略（MIME包装器与部分客户端会带；RFC 2046「implementations must ignore anything that appears before the first boundary delimiter line」）；无preamble时正文第一行就是`--boundary`也合法（BNF里`[preamble CRLF]`本可选；Werkzeug与Rack的正则都把前导换行做成可选，Go在`partsRead == 0`时直接跳过非边界行）；
- 换行逐处容忍`\r\n`、`\n`、`\r`三种（Werkzeug的`LINE_BREAK`同此）；不采Go式「首部件观察一次即全局切换LF模式」——混用换行的畸形body后半会失配；
- 部件定界＝换行＋`--boundary`，正文止于定界行之前的那个换行（RFC 2046「the initial CRLF is considered to be attached to the boundary delimiter line rather than part of the preceding part」；把这两个字节算进正文，每个字段值就多一个`\r\n`，与Go/浏览器产物互操作即坏）；终结＝`--boundary--`＋行尾水平空白＋可选换行，其后epilogue忽略（Blink在闭合线后总再写一个`\r\n`，Go`isFinalBoundary`亦容忍）；无终结→400（body已在手，不存在「数据还没到」的歧义，静默收下只会掩盖客户端bug）；
- 部件头逐行`Name: value`，头名大小写不敏感（承建事实：`HttpHeaders`本就是小写键表，`stdlib/http.n:23`），参数不假设顺序；obs-fold续行（以空格或制表开头的续行）并入前一条的值（RFC 7230 §3.2.4给服务器的两个合法选项之一，Werkzeug`HEADER_CONTINUATION_RE`与Go的`textproto`都取此项）；部件头总长上限`DEFAULT_MAX_PART_HEADER_BYTES`＝16 KiB（busboy从node带来的硬编码`MAX_HEADER_SIZE`），`name`参数值的上限`DEFAULT_MAX_FIELD_NAME_BYTES`＝100字节（调研注§7采纳项，busboy`fieldNameSize`与multer同为100），两者都是文法级拒绝、越限400且消息具名；
- `Content-Disposition: form-data`必须存在且带非空`name`，缺一→400（RFC 7578 §4.2的两个MUST；Go把无名部件`continue`跳过，等于把客户端错误咽成「少一个字段」，比报400更难排查）；正文长度为0的部件合法；
- 有`filename`参数即视为文件部件（RFC 7578 §4.2），`filename=""`也算文件（存在性阵营：Werkzeug按参数存在性分流、WHATWG fetch同口径；Go与Rack把空串降成字段，那是「用户清空文件框」时应用层才需要的猜测）；
- 部件无`Content-Type`时登记缺省`text/plain`（RFC 7578 §4.4与WHATWG fetch解析一致；邮件语境的缺省charset仍是US-ASCII——RFC 6657 §4——但本版文本正文一律按UTF-8解码，不据该缺省转码）；
- 除`Content-Disposition`与`Content-Type`之外的部件头一律忽略（RFC 7578 §4.8「MUST NOT be included and MUST be ignored」——报错拒绝反而违RFC）；`Content-Transfer-Encoding`不透明直通、不解码（§4.7已废且「no deployed implementations … discovered」，九.20）；
- `filename`剥目录路径、其余字符一律不碰（剥法与理由见D-20），`Content-Type`参数值原样保留；两者都不自动percent解码（RFC 7578 §2只是「MAY」，HTML构造侧「The user agent must not perform any other escapes」，线上默认就是原始字节；擅自解码会把真叫`%20a.txt`的文件改坏，要解码由应用显式调`url.percentDecode`）；
- 不递归解析嵌套multipart（RFC 7578 §4.3已废该用法），带multipart类型的部件按不透明字节处理（九.4）；
- 文本部件正文一律按UTF-8解码（WHATWG fetch固定UTF-8，是现代浏览器与fetch系的实际语言；RFC 7578 §5.1.2的部件`charset=`与`_charset_`梯度服务HTML form遗留场景，本版无既有用户，九.21），二进制字节直通，绝不经字符串往返；
- 同名部件不合并、结果保持出现序（RFC 7578 §5.2「Form parts with identical field names MUST NOT be coalesced」、HTML「Multiple entries with the same name must be treated as distinct fields」），故`FormData`是保序多值列表而非覆盖式字典（6.9）。

理由：每条都有真实客户端差异或RFC明文可依（取证见`temp/ref/multipart_research.md`第一、二、九节）。宽容清单——无前导CRLF的首定界、`\n`/`\r`换行、行尾水平空白、quoted边界、obs-fold、epilogue——逐条要么有RFC收件人义务（2046 §5.1「must ignore」「receivers MUST」、7230 §3.2.4选项二），要么有两家以上主流实现背书——浏览器侧的`multipart/form-data`解析至今没有规范标准（WHATWG fetch自认「The above is a rough approximation of what is needed for multipart/form-data, a more detailed parsing specification is to be written. Volunteers welcome.」），故「与两家以上主流实现一致」已是可得的全部判据，不是退让；拒绝面只留在文法错误本身：缺boundary、多boundary参数、缺`Content-Disposition`、缺或空`name`、字段名超长、无闭合线、部件头超长，一律400。代价：混用换行等畸形形态也会被收下，宽进严出的界线由这份400清单钉死，解析器不猜「客户端本想表达什么」。

### D-20 文件名：回显原样、落盘清洗、加随机后缀

`FormFile.filename`是**剥掉目录路径后的原样值**：解析层把`/`与`\`都当分隔符取末段（Rack的`split(/[\/\\]/)`同此；Go`filepath.Base`按宿主平台判定是半修法——POSIX宿主上`C:\..\x.png`整串成为文件名，应用再拼接就成了Windows式穿越），故`C:\fakepath\x.png`进来就是`x.png`；除此之外一律不碰——`..`、控制字符、非ASCII、百分号都留着供回显与校验。剥路径归解析层是收件人义务（RFC 2183 §2.3「treated as a terminal component only」、RFC 7578 §4.2「do not use the file name blindly … do not use directory path information」），也是唯一与用户策略无关的一步：任何一处把`filename`参与拼接的应用都需要它，放到调用方就有人先拼后洗。`safeName()`在此之上做存储侧清洗：去NUL与控制字符（NUL进C系栈会截断扩展名，Rack为此`gsub("\0", '%00')`）、丢`.`与`..`、Windows保留设备名（`CON`/`NUL`/`PRN`/`COM1`…，名单见Werkzeug`security.py`）加`_`前缀、折叠空白、截`DEFAULT_MAX_NAME_BYTES`＝120字节（OWASP《File Upload》的「Set a filename length limit」；120给扩展名与随机后缀留头，也远低于各文件系统的255字节上限）——取末段那一步在`safeName()`里保留，作幂等防御，护的是从别处（表单字段、命令行、外部接口）送进来的未剥路径值；`saveTo(dir)`用`safeName()`＋4字节随机后缀hex落盘；`saveAs(path)`由调用方给完整路径、框架不清洗。
理由：「回显要忠实、落盘要安全」是同一条经验的两半——RFC 2183 §2.3给接收方的义务是「treated as a terminal component only」、RFC 7578 §4.2是「do not use the file name blindly … do not use directory path information」、RFC 7578 §7另点名「not inadvertently overwrite files」；Werkzeug`secure_filename`（NFKD转ASCII＋设备名前缀`_`）、Django`get_valid_filename`（更弱、不处理设备名）与Rails ActiveStorage`sanitized`（危险字符换`-`）都在**存储命名处**清洗且策略互斥，故策略性清洗做成`FormFile`上的opt-in方法而非解析层动作（解析层只承担与策略无关的剥路径，见上）：应用要换策略（扩展名白名单、哈希名）时不必先跟解析器搏斗。加随机后缀同时防同名覆盖与目录内枚举。代价：`saveTo`产出的名字不可预测，需要「原名入库」的应用自行拼接并落自己的清单；扩展名白名单与类型核验属应用策略（OWASP《File Upload》：「List allowed extensions」「don't trust the Content-Type header as it can be spoofed」），框架不代做。

### D-21 multipart构造侧：`MultipartBody`

`addField`/`addFile(field, filename, contentType, data)`/`contentType()`/`toBytes()`。boundary取`math.randomBytes`的20字节转40位十六进制（Go`randomBoundary`即crypto/rand 30字节→60位hex的同型做法；只用十六进制是为了躲开Blink注释记录的坑——「the following characters, though legal, cause some sites to fail: `(),./:=+`」，crbug 575779，浏览器因此只用字母数字），并在`toBytes()`时自检正文不含行首该boundary，撞上则重生成。CRLF纪律照Go`CreatePart`/`Close`：首部件写`--boundary\r\n`，其后每个部件之前写`\r\n--boundary\r\n`，头行后补一个`\r\n`空行，收尾写`\r\n--boundary--\r\n`（闭合线之前的CRLF属于定界行本身，见D-19）。`addField`不带`Content-Type`（HTML编码算法明文「The parts of the generated multipart/form-data resource that correspond to non-file fields must not have a `Content-Type` header specified.」，Go`CreateFormField`同此）；`addFile`的`contentType`为空时填`application/octet-stream`（RFC 7578 §4.4的SHOULD，Go`CreateFormFile`同此），且字段部件永不送`filename=""`（要么带真名，要么不带该参数）。参数值转义只取HTML子集：`"`→`%22`、LF→`%0A`、CR→`%0D`，不转义`\`（WHATWG「The user agent must not perform any other escapes」；Go`escapeQuotes`用`\`转义`"`并称不转义会被Chrome当转义符，两家取向相反，本版按标准侧走，与D-19的解析侧对称）。
理由：客户端上传（调外部API）与服务端解析必须共用同一套CRLF纪律与boundary形状，只做一侧会留下「能收不能发」的半成品；它同时是本版multipart测试的地基——没有构造器就只能手工写字节串做回归，易腐。随机boundary碰撞概率极低，但仍做一次行首扫描自检（RFC 2046要求boundary不得出现在封装内容中；内存内一次扫描成本可忽略，Go只靠概率，扫一次是把不确定性换掉）。代价：`toBytes()`整体驻留内存（与D-18同一姿态）。

### D-22 优雅关闭：三态状态机＋登记表＋`drain`/`close`分工＋复位

`HttpServer`加`m_state`（0=serving、1=draining、2=closed）与连接登记表（`Dict<int, HttpConn>`＋`Mutex`＋自增id），公开面变更如下：

- `int drain(int graceMs = DEFAULT_DRAIN_GRACE_MS)`：置draining→关监听器（唤醒accept并让其退出）→此后写出的每个响应一律盖`Connection: close`（落地只改一行：`serveOne`里既有的`res.connectionClose`汇总行`stdlib/http.n:1468`加一项`!m_srv.isServing()`，它跑在`writeResponse`（`:1469`）之前；`writeResponse`据此在写响应头之前落`Connection: close`（`:1246-1251`，`writeHead`在其后的`:1252`），五条写分支因此都带上它。`HttpHeaders.set`是替换语义，处理器自设的值让位，SSE自设的`Connection: keep-alive`也不例外——本次流仍跑完，只是不再复用该连接；`connectionClose`全库没有复位点，被置上的连接不会回到复用态）→登记表内**当前没有请求在处理中**的连接（即停在keep-alive等待里的空闲连接）立即强关→每连接在**读下一请求之前**检查`isServing()`并跳出循环（已建立的连接不再吃新请求，在途请求跑完再走）→`graceMs`内**在途请求计数**`m_inFlight`归零则返0，超时后对登记表残余连接逐个强关并返残余数；
- 新增`m_inFlight`（`AtomicInt`）：加于`serveOne`调用处理器之前、**减于`serveOne`返回之后**，它才是排空要等的量。窗口是整个`serveOne`而不是处理器那一段：响应写出（`:1469`）与分块泵（`:1261-1262`）都跑在处理器返回之后的同一条连接线程上，都在窗口之内，因此正在推流的SSE连接不会被判成空闲而被主动关闭，排空也不会在一帧写到一半时返0。既有`m_active`按**连接**计数（accept处加，`stdlib/http.n:1582`；连接线程`run()`结束才减，`:1395`），一条停在keep-alive等待里的浏览器连接会把非零值保持到`keepAliveTimeoutMs`＝30000ms（`:1517`）为止，而它正停在`readRequest`里、根本走不到请求循环顶部的`isServing()`检查（`:1366`）——只看`m_active`的排空必然烧满宽限、把空闲连接强关掉、再把它们报成残余，与「真有请求被卡住」无从区分。登记表内每条连接另带一个「本次请求处理中」标志（与`m_inFlight`同处加减、同一窗口），`drain`据此区分空闲与在途；
- 空闲连接的强关由`drain`一侧发起，「判为空闲」与`forceClose()`在登记表同一把锁内做完，不给连接线程在这两步之间领走新请求的窗口。被关的连接此刻多半parked在`readRequest`的`fill`里（`stdlib/http.n:1107`→`:909`→`:985`的`peekFilled`），而native侧把这种竞争定成「关者胜出」：parked的读以`kNetErrClosed`返回并在那条路径上抛`SocketException`（`src/native/net/net_tcp.cpp:171-174`＋读路径`:252-253`；`stdlib/net.n:55`映射、`:424-425`抛出），连接线程随即按异常经`run()`的外层catch收尾、走到既有的`m_srv.connDone()`注销（`stdlib/http.n:1361-1396`）。因此排空不靠唤醒通道、也不靠等它的`keepAliveTimeoutMs`；循环顶部的`isServing()`检查与登记表的主动强关是互补而非重复：前者只管已经走到请求边界的连接，后者专管停在等待里走不到边界的连接。
- `void close()`：立即——置closed→关监听→登记表逐个强关→等在途工作线程退出，等待上界`DEFAULT_CLOSE_JOIN_MS`＝2000ms。原「只关监听器、在途连接自生自灭」的语义不再保留；
- 两个入口都可重复调用：已不是serving就不再改状态；`drain`在已draining时继续等剩余宽限、在已closed时**立即返0**（残余已在`close()`里结算过，再等一个整宽限只是白等），`close`直接返回；
- closed同时是「没有任何东西在跑」的基线态：构造后从未`start()`的实例，与`start()`在`bind()`处抛出（端口被占是调用者会接住的寻常结果）的实例，都同已关闭的实例走同一答案。落点是构造器与`start`/`serve`的复位段置closed、`bind()`返回之后才升为serving，于是两个入口全程不必碰`m_listener`；
- `m_state`取代既有`m_closing`旗标而非并存（`:1527`声明、`close()`置位`:1563`、accept循环读取`:1575`）：三态已涵盖旧旗标的全部用途（「别再接新请求」＝非serving），accept循环的退出条件改读`m_state != serving`，两个旗标并存则可能出现「`m_state`说serving而`m_closing`说1」的分叉；`start`/`serve`开头复位因此只处理`m_state`这一项状态，另加`m_rejecting`/`m_active`/登记表归零；`m_listener`非空即抛具名`Exception`禁双启动；
- 新增`bool isServing()`：SSE与长轮询handler可轮询退出条件。

缺省宽限`DEFAULT_DRAIN_GRACE_MS`＝15000ms的取法：慢客户端上传与常规处理器收尾都在秒级，15 s足以覆盖；它须低于容器编排器默认的进程退出总宽限（Kubernetes`terminationGracePeriodSeconds`缺省30 s，即调研注§6那对双层时限的外层，「`drain-time-s`小于退出总宽限」的同款关系）；不必与http.n的30 s keepAlive超时挂钩——空闲连接是被主动关闭的，不用等自己的keepAlive超时；同时明显大于`DEFAULT_CLOSE_JOIN_MS`＝2000ms，保证`drain`超时后的强关路径仍留完整余量走完join等待。

接线点全部落在既有位置，不新增循环与回调：登记位置只能在容量分支之后、`thread.spawn(conn.run)`之前（`stdlib/http.n:1596`），不能在`m_active`自增处（`:1582-1583`）登记——超出`maxConnections`的两条退路（`:1584-1595`）只回退计数便`continue`，既不进`run()`也不走`connDone()`（`reject503`的`finally`还的是`m_rejecting`，`:1419-1422`），在那里登记等于每条被拒连接留一条永不注销的表项，`drain`的逐个强关与残余数会把死套接字算进去、过载服务器上登记表还只增不减；`isServing()`检查插在`HttpConn.run()`请求循环顶部（`:1366`）；注销挂在`run()`末尾既有的`m_srv.connDone()`（`:1395`，连接id随该调用带回）。排空是否完成看新增的`m_inFlight`（加减的窗口是整个`serveOne`，含写出与泵，见上），`m_active`只用于`maxConnections`判定（`:1526`、`:1582`），两者语义不同、不可混用；登记表则为「主动关空闲连接」与超时强关所必需，并携带每连接的「本次请求处理中」标志。强关必须新开一口`HttpConn.forceClose()`（直接关`stream`）：既有`HttpConn.close()`是有序关闭（半关＋排空＋最多`CLOSE_GRACE_MS`，`:1292-1296`），超时路径不能再等它。登记表的形态沿用在树的客户端池先例（`stdlib/http.n:1624`、`:1702-1747`），只是把清单换成按id索引的`Dict`以免每次连接结束都整表拷贝；登记与注销的读写都在登记表自带的`Mutex`内，`HttpServer`是普通`class`，故直改字段即可，不必照搬客户端池那套「读进本地再写回」的快照写法（探针P6实测普通类容器字段就地改动生效）。公开面不新增`isDraining()`：`isServing()`（6.10）在draining与closed期间同为假，而「盖`Connection: close`」与「不再吃新请求」两条规则在此期间同时成立，于是两处内部判据共用这一个公开谓词——既免去私有`m_state`的跨类访问，也不留下两个谓词各说一套的余地。

理由：roadmap原文要求「连接登记表＋排空/强关」＋复位项。`close()`改强关是因为「服务器已关、进程仍在等一条赖着的keep-alive」在教学示例里最难解释，而把排空与强关分成两个入口已是事实标准（Go`Server.Shutdown`与`Close`）。排空入口不沿用它`Shutdown`的叫法：`shutdown`与`close`在英文里近乎同义，谁排空谁强关只能靠记，这正是本次要消掉的混淆（用户裁定2026-11-17）。`drain`一词一是roadmap原文的「排空」，二是同类系统的通用词（nginx`QUIT`、Envoy与Kubernetes的drain语义），三是与强关语义正交——一个「等在途走完」、一个「现在就断」。释放动词不换叫法（同场议过`terminate`）：`close()`是全库唯一的释放动词，12处声明跨四包（3.1实证），全库公开面无`terminate`/`kill`/`disconnect`，而`terminate`在主流用法里收束的是进程与线程、不是句柄；让`HttpServer`成为唯一例外，等于给「开什么就关什么」这条贯穿手册的纪律加一条脚注。建立侧动词本就按对象种类分（`bind`/`accept`/`connect`/`start`），要统一的只有释放侧，故`web`层不新增任何打开动词（`App.listen`/`serve`沿用`HttpServer.start`），`Sse.close()`同理。复位项不改则`close()`后重新`start()`的accept线程会因残留的非serving状态在下一次accept失败时立刻退出（3.1实证），且旧accept线程在双启动下永不退出。closed做成「什么都没在跑」的基线态、而不是在两个入口里判`m_listener`是否为空，是因为清理路径上的调用（含`web.App`的`drain`/`close`转发，6.2）不该先问私有细节：状态机里有这么一个真状态，转发就能无条件做，未启动与`bind()`失败两种情形也自动与已关闭同形。代价：`close()`语义变更（不向下兼容）；SSE这类长连接只有强关路径，手册与示例须写明「先`drain`排空、必要时再`close`」；`drain`与同包的`HttpBodyReader.drain(int)`（`stdlib/http.n:842`）同词不同receiver，手册以类名限定语区分；排空期被主动关闭的空闲连接可能正撞上客户端已发出的下一个请求，按HTTP的幂等语义（RFC 9110 §9.2.2）幂等请求可安全重发，Go`Shutdown`也是同一形态。

### D-23 `net`收尾：`isEof()`新面＋已满足项如实登记

roadmap所列「`TcpStream.read`区分超时与对端EOF」**现况已满足**：`read`对端EOF返0、超时抛`TimeoutException`（`stdlib/net.n:292-309`的契约），且已由`tests/e2e/stdlib_net_tcp_read_timeout.n`与`tests/e2e/stdlib_net_window.n`（超时腿与EOF腿分别钉住）覆盖，本版只补一枚把三种读法放在同一张矩阵里的对照测试，不改行为。真正的二义在`readLine`（空行与EOF都返`""`，源码注释自陈原因是「a string has no null value」，`stdlib/net.n:455-468`）与`readExactly`（短读抛`SocketException`，`:520-546`）：本版新增`bool isEof()`，纯源码读取既有的粘滞标志`m_winEof`（`:343`），与`isClosed()`（只看本地已关——native侧「槽位缺失或已标记关闭→1」，`src/native/net/net_tcp.cpp:394-400`）正交；手册把三条读面的EOF/超时矩阵写全。
理由：区分已成立，就不虚构工作（如实登记也是设计的一部分）；`isEof()`是消除二义的最小正交面——比给`readLine`加`out bool`形参更不动既有签名，也不新增异常种类，且粘滞标志已在源码里，无需宿主改动。代价：`isEof()`只报「已观测到的EOF」，未读过就断连的场景仍要靠一次`read`才置位（手册明写）；`readLine`返空串本身仍是二义。

### D-24 异常策略：不新增异常类

请求期可预期失败（415/413/400/404/405与`next()`重入）一律`http.HttpException(消息, 状态)`；启动期配置错误（路由冲突、静态根不存在、挂载前缀非法、双启动）一律普通`Exception`带具名消息，期望冒到`main()`终止启动。
理由：`HttpException`已携`status`（`stdlib/http.n`的const类），再造`WebException`会让两套状态码语义并存；启动期错误没有HTTP状态可言。代价：调用方若要区分「框架的400」与「自己抛的400」，得读消息文本。

### D-25 并发姿态

`App`与`Router`的注册面在首请求之后进入只读：再注册路由或中间件抛具名`Exception`。组件的配置字段不受此限（`StaticFiles`的`index`/`maxAgeSeconds`/`useETag`、`FormOptions`三字段）：它们在每次请求内直读，运行期改动只影响其后的请求，改与不改由用户自担。会话store整体`Mutex`保护，单个会话不加锁（并发写同一会话为last-write-wins，手册标注）；中间件实例跨请求共享，故必须无状态或把每请求状态放`ctx`的取值面（`set`/`get`），手册要求、示例示范。
理由：每连接一虚拟线程是http层既定模型（v0.13.5 D6），框架再加per会话锁的代价不抵收益（会话内并发本是低频场景，express-session同样不锁）；运行期改路由表在多线程下会带来读侧撕裂，KISS。注册面之所以要守、配置字段之所以可以不守，差别在一致性：路由表是「精确表＋有序模式表」两处结构的组合，读侧必须看见一致快照，否则同一次分派会跨两个版本；单个配置字段是单次直读，读到的新旧值都自洽。代价：热重载/动态路由场景需要另建机制（届时按`Router`子类或副本切换设计）。

### D-26 承载边界

零编译器/VM改动；新增native三枚（`math.randomBytes`、`fs.writeBytes`、`fs.modifiedTimeMs`，落在`src/native/math`与`src/native/fs`）＋`fs.readBytes`返回类型变更；`net.isEof()`与`http`的关闭状态机都在源码层，宿主不动；NCU/npkg线格式与版本不动（无新记录种类）；`web`包的模块清单由预置包的源文件glob收编，子模块要求把单层glob改成递归收集并让构建期拷贝保留目录结构（`CMakeLists.txt:267`的`file(GLOB …)`与`nlang_stdlib_package`目标的拷贝命令，见D-28与第八章）；`web`包import `http`/`url`/`json`/`io`/`fs`/`math`/`system`/`thread`/`concurrent`（`math`是D-13/D-21的`randomBytes`所需，漏import则会话id与boundary在编译期就是未解析符号）——stdlib内跨包import是既有做法（`stdlib/http.n:7-13`即七行逐行import），包内文件怎么切分见D-28（2026-11-19改判，本条原先据「标准库不能是多文件包」要求单文件）。
理由：本版的承载边界只钉「宿主与线格式不动」，包内文件怎么切不在它的约束之内；预置包的收集规则是一处构建脚本而非宿主能力，改它属承载边界内的一次必要改动。代价：`CMakeLists.txt`的预置包收集与拷贝要改成递归形态，随第一枚子模块落地；既有那条「删了stdlib源文件要手工删build树里的陈旧拷贝」的告诫随之覆盖子目录。

### D-27 本版语言层面：无新语法，另有两处名称解析缺陷当版改正

无新语法、无新关键字。本版实测的四条可用性缺口登记进roadmap「远期特性」的语言层面缺口清单，不用绕行掩盖：带默认值的func形参上下文中函数引用被拒（P2，探针F3实测rc=1）；实例构造调用的实参位置不给函数引用预期类型（`FuncMiddleware(handler)`一类`Type(args)`写法，自由函数引用与绑定方法引用一起被拒，同一枚引用在普通调用实参、字段赋值与带类型的局部声明三处都能绑定，P2，探针`temp/t6_probe3.n`实测2026-11-19；与第一条同属期望类型的传播缺口）；下标元素不能就地调用（`mws[0](ctx)`报语法错，必须先取进同类型局部变量，P7）；`Dict`缺`getOrDefault`而`get`缺键抛异常（P4）。
理由：框架侧的单名重载与带默认值读取面只是**可用**，不等于**合理**；缺口该在语言层面解决就该在语言层面排期（既有裁定：语言层面缺口合理时直接补语言）。代价：本版用户的func形参不能带默认值（注册方法的func形参因此一律不带默认值，见6.2），从容器取出的函数引用必须先落进同类型局部变量才能调用，要交给实例构造调用的函数引用也要先落进带类型的局部变量（`App`构造`m_routeLink`即此形制）。另有两处名称解析缺口（限定名的类静态成员、库包通配导入）不在上面这份递延清单里，本版按D-29改正编译器（用户裁定2026-11-19：现有VM或编译器不合理处先改正，而不是迁就现有实现，不需要向下兼容）。

### D-28 `web`包的多模块分层

`web`是一个点分多模块包：包级模块`stdlib/web.n`装`App`、`FuncMiddleware`、`Middleware`、`RequestContext`、`Router`与`Session`/`SessionStore`/`MemorySessionStore`；三枚子模块`stdlib/web/static.n`（`StaticFiles`）、`stdlib/web/sse.n`（`Sse`）、`stdlib/web/form.n`（`Form`、`FormOptions`、`FormData`、`FormFile`、`MultipartBody`）只单向import包级模块，包级模块不import任何子模块，子模块之间也互不import。用户引用子模块的类型要再import那一枚子模块或写`import web.*;`（6.1）。

理由：分层边界不按功能目录美学画，由编译器给的一条硬约束推出——包级模块import自己的子模块就是import cycle，编译期硬拒（探针`temp/nest/`：`k.n`与`k/w.n`互import，原文`Error: import cycle detected: k -> k.w -> k`），而任何要用`RequestContext`或`HttpResponse`的组件必然import包级模块，于是**包级模块的公开面所点名的类型只能留在包级模块内**。`App`与`RequestContext`是框架的两个入口，`web.App`/`web.RequestContext`这两个拼写就是D-2的公开词汇（也是已落地的中间件语料与6.2/6.3的签名），它们点名`Middleware`（中间件接ctx、`ctx.next()`推进链）、`Router`（`mount(string, Router)`与各注册方法的转发）、`Session`与`SessionStore`（`ctx.session()`、`useSessions(store)`、`App.handle`尾部回写`Set-Cookie`），因此这几族同层。`StaticFiles.serve(ctx)`、`Sse`逐帧写`HttpResponse`、`Form.parse(ctx)`都只单向点名核心，所以能出去。反向方案是把`App`自己也挪进子模块：那会让入口的限定拼写变成`web.app.App`，用户只`import web;`时拿不到入口，代价由每个用户代码付，而不是由框架的文件行数付。包级模块因此仍在1200行级（管线族＋会话族），由D-1的类边界与源规模守卫控住；三枚子模块各预计200至600行，且都是落地后很少再改的叶子。

代价：公开词汇按模块拼（`web.static.StaticFiles`、`web.form.FormData`），只写`import web;`的用户引用不到子模块——探针里只`import k;`的那份消费者实测报`Module 'k.w' is not imported. Add 'import k.w;' (or 'import k.*;')`，手册必须把这条import形态写清（第八章）。行数收益因此集中在三枚叶子而非均摊到全部14枚公开类。

### D-29 限定名与库包通配导入的解析修复

`web`改成点分多模块包（D-28）之后，用户要写`import web.*;`与`web.form.Form.parse(ctx)`两型拼写，而这两型今天就编译不过，同形状的其它拼写却都能编译。实测矩阵（探针在`temp/dlib/`、`temp/dmod_b/`、`temp/dmod_c/`、`temp/d29p/`，编译器取`build-dev`的`ncc`，因而按stdlib预置包语义解析库包；引文一律省略`文件(行, 列):`前缀）：

| 拼写 | 实测 |
| --- | --- |
| `import http;`之后`http.HttpResponse res = http.HttpResponse()` | 通过 |
| `import http;`之后裸类名静态方法`HttpResponse.reasonPhraseFor(404)` | 通过 |
| `import json;`之后限定名枚举成员`json.JsonKind.Object` | 通过 |
| `import json;`之后在限定名枚举成员上调方法`json.JsonKind.Object.toString()` | `Error: Module 'json.JsonKind.Object' is not imported. Add 'import json.JsonKind.Object;' (or 'import json.JsonKind.*;') at the top of this file.`＋`Error: Incompatible type "json.JsonKind.Object.toString()".` |
| 项目模块`import lib;`之后同一深度`lib.Color.Blue.rank()`与`lib.Color.Red.toString()` | 通过（探针`temp/d29p/p2/`退出码0；这两型正是手册`04-types/enum.md`§4.6.4中英两份示例给出的拼写） |
| `import http;`之后限定名静态方法`http.HttpResponse.reasonPhraseFor(404)` | `Error: Module 'http.HttpResponse' is not imported. Add 'import http.HttpResponse;' (or 'import http.*;') at the top of this file.`＋`Error: Incompatible type "http.HttpResponse.reasonPhraseFor(404)".` |
| `import json;`之后限定名静态常量`json.JsonValue.MAX_DEPTH` | 同一形制，落点换成`json.JsonValue` |
| 照该建议改`import http.HttpResponse;` | `Error: Module 'http.HttpResponse' not found. Check the project Sources list or -I import path.` |
| 照该建议改`import http.*;` | `Error: Wildcard import cannot target library package 'http'. Use 'import http;'.` |
| 项目模块`import p;`之后`p.K.s()`与`p.K.TAG` | 通过（探针退出码14） |
| 项目模块`import m2.*;`之后`m2.side.Widget.answer()` | `Error: The function "answer" does not exist or is not accessible.` |
| 项目模块`import m2.side;`之后同一拼写 | `Error: Module 'm2.side.Widget' is not imported. Add 'import m2.side.Widget;' (or 'import m2.side.*;') at the top of this file.` |

三条补救建议没有一条可用：补精确import得到「not found」（类不是模块），补通配import被编译器自己拒绝，照抄限定拼写则原地报错。项目模块与库包之间、一级与二级之间、枚举成员与类静态成员之间的这些落差都不是设计，而是解析器三处认领臂各按自己的谓词抢下这条限定链（`src/compiler/builder/ExprResolverStdLib.cpp`）：库包认领臂只看链首是不是库包名（`:90`），子包否决扫描在第一个已知前缀就返回（`:108-126`，于是`m2.side.Widget`在`m2`那一层被误认领，`m2.side`那一层的类型判定再没人看），限定值与限定类型引用两处的闸门同样只认「首段是库包」（`:307-310`、`:396-399`），而限定值那条还只认枚举成员（`:319-320`），且该例外只到成员值本身：链再多一段（在成员常量上调方法）就被模块路径认领臂抢下——手册`04-types/enum.md`§4.6.4示例给出的`lib.Color.Blue.rank()`在项目模块里通过，换成库包里的`json.JsonKind.Object.toString()`就报`Module 'json.JsonKind.Object' is not imported`，报错文本自证是模块路径认领臂把这条四段链当成了模块路径。同一文件对限定类型引用的注释（`:377-385`）写明限定拼写「与裸类名共用同一条桩记录与同一套诊断」，且该路径要在绑定类型之后交给通用类型接收路径「在类型作用域内解析成员或静态调用」——统一限定访问是既定设计，现状是实现没跟上。

本版把规则统一成三条：

1. **限定名按最长模块路径优先、其次类型分派。**`a.b.c.m`若`a.b.c`是本构建已知的模块（项目模块、按搜索路径内联进构建的库源文件、或库索引里的库模块），`m`按模块成员解析（自由函数或再深的子模块）；否则取某个已知前缀`a.b`，若`c`是该模块内容许本单元访问的类型，`c.m`就在该类型作用域内解析——静态方法、静态常量与枚举成员同等，在枚举成员常量上调方法亦然，库包与项目包同等、任意嵌套深度同等。同名类型优先于同名子模块的既有优先级不变（编译器内称m12优先级规则，落在`ExprResolverStdLib.cpp:101-107`的注释与其上的decline单测）。
2. **`import p.*;`一律并入`p`及其全部子模块，库包与项目包同等。**扁平库包的通配因此与精确import等价，`import http.*;`不再是硬错误；`import web.*;`一次打开`web`、`web.static`、`web.sse`与`web.form`。
3. **诊断只对确实是模块路径的名字说「is not imported」。**指向类型的成员缺失按成员不存在表述，指向不存在模块路径的沿用既有`… not found. Check the project Sources list or -I import path.`形制；给用户的每条补救建议本身必须可用。

理由：这不是加语法，而是修一处无法向用户解释的分裂——同一段代码在项目模块里编译通过、放进库包就不通过；枚举成员经限定名取得而类静态成员取不到；编译器给出的补救建议还被编译器自己拒绝。现行绕行写法（静态成员一律裸类名，实施计划裁决40）会把缺陷固化成手册里一条「限定名只到类」的特例：用户照`web.App()`的直觉写`web.form.Form.parse(ctx)`必撞错，而D-28选定的多模块词汇在库包通配导入被拒的情况下只能逐枚精确import。还有一处落差是手册已经写下的承诺：`04-types/enum.md`§4.6.4的中英示例都给出`lib.Color.Blue.rank()`与`lib.Color.Red.toString()`，同一拼写在项目模块里成立、在库包里必败，用户照手册写就撞错。按既定裁定（语言层面缺口合理时直接补语言而非绕行、不向下兼容、现状不是设计依据）与用户裁定「现有vm或编译器不合理的先改正，而不是迁就现有实现，不需要向下兼容」（2026-11-19），本版改正编译器。

代价：解析侧三处认领臂换成一条共享谓词（模块路径存在性＝登记表∪库索引，类型否决＝扫描全部前缀而不是停在第一个已知前缀）；`Wildcard import cannot target library package`这条诊断随规则2移除，钉它的单测`tests/test_compiler/test_module_import.cpp:659`改写成「通配并入库子树」的断言；库源发现层要为通配枚举库目录子树（`src/compiler/ModuleBuilderImports.cpp:302`今天遇通配直接跳过）；手册两处中英成对更新——`docs/user_manual/{en,zh}/specification/03-program-structure/index.md`§3.3那句「`p`命名库包即编译期错误，因为库包是单个单元」的前提已被嵌套库包推翻（D-28），§3.6的限定名段补一句经模块路径限定类型之后的成员访问。语法、关键字、字节码、ABI与退出码都不变：修复只让此前必然编译失败的拼写成立，既有程序的语义与诊断不改写，裁决40的裸类名写法继续有效、只是不再必需。另两份dev文档随本版对齐：`docs/dev/name_resolution_design.md`§5的V3把「库包禁通配」列为保留项，其自陈理由是分工「显式＝逐模块点名、通配＝子树路径准入」——扁平库包没有子树，通配相较精确import无可增益，故禁；D-28让库包真实拥有子树之后该前提消失，该条限制随规则2撤销，V2的限定名准入则按规则1扩到类型成员，该文档加一条带日期的修订记录交代这次撤销与扩展的理由。手册侧`03-program-structure/packages.md`§3.9「包声明的一切都可以通过限定名访问」与`04-types/enum.md`§4.6.4的示例本就按统一形态书写，修复后成为事实，两页无需改动。

## 六、API面

以下为签名级草案，语义与错误姿态见第五章；参数默认值以`DEFAULT_*`常量为名进实现（magic number必须常量化）。

### 6.1 引入与公开词汇

`import web;`给出包级模块的公开类：`App`、`RequestContext`、`Middleware`、`Router`、`Session`、`SessionStore`、`MemorySessionStore`。三枚子模块各有自己的限定名，引用它们要再import那一枚子模块或写`import web.*;`：`web.static`有`StaticFiles`，`web.sse`有`Sse`，`web.form`有`Form`、`FormOptions`、`FormData`、`FormFile`、`MultipartBody`；为什么这样分层见D-28。适配器`FuncMiddleware`与常量表为包内实现细节，不进手册。用户侧签名一律限定名（D-2），跨子模块的示例与语料统一写`import web.*;`（6.11）。静态成员既可经裸类名也可经模块限定名访问（`Form.parse(ctx)`与`web.form.Form.parse(ctx)`同样合法，D-29）。

### 6.2 `App`

```
class App {
    public App();                                        // 默认内存会话store + 文本404
    public void use(func<void, RequestContext> fn);       // 函数风格中间件（D-3）
    public void use(Middleware m);                        // 子类风格中间件（D-3）
    public void on(string method, string pattern, func<void, RequestContext> h);
    public void get(string pattern, func<void, RequestContext> h);
    public void post(string pattern, func<void, RequestContext> h);
    public void put(string pattern, func<void, RequestContext> h);
    public void patch(string pattern, func<void, RequestContext> h);
    public void del(string pattern, func<void, RequestContext> h);
    public void head(string pattern, func<void, RequestContext> h);
    public void any(string pattern, func<void, RequestContext> h);
    public void mount(string prefix, func<void, RequestContext> h);   // 剥前缀（D-10）
    public void mount(string prefix, Router r);           // 子路由分组（D-10）
    public void setNotFound(func<void, RequestContext> h);
    public void onError(func<void, RequestContext, Exception> h);     // 覆写500呈现（D-11）
    public void useSessions(SessionStore store);
    public http.HttpServer server();                      // 调http层参数（maxBodyBytes等）
    public http.HttpResponse handle(http.HttpRequest req);  // 交给HttpServer的处理器
    public void listen(string host, int port);            // HttpServer.start + handle
    public void serve(string host, int port);             // 阻塞式
    public int drain(int graceMs = DEFAULT_DRAIN_GRACE_MS);   // 排空，返残余连接数（D-22）
    public void close();                                      // 立即强关
}
```

`use`与`mount`都是单名重载：同名候选里只有一个是函数类型时，函数引用照样绑到它（探针X1–X4实测exit 0，见3.2的P2行），两个名字纯属给用户加记忆负担（用户裁定2026-11-17）。func形参一律无默认值——带默认值的func形参会让函数引用找不到期望类型（P2实证形态，探针F3实测rc=1）。`server()`让「框架的注册面」与「传输层的上限与超时」各归其位，不必为每个http参数在`App`上再开一遍转发。

### 6.3 `RequestContext`

```
class RequestContext {
    public http.HttpRequest req;
    public http.HttpResponse res;
    public string method;                       // 请求方法
    public string path;                         // 剥去命中条目前缀后的原始路径（未解码；恰等于前缀时为"/"，D-10）
    public string query;                        // 原始query串（未解码）
    public string param(string name);           // 路径参数，已percent解码；缺→""
    public int paramInt(string name, int def = 0);
    public bool hasParam(string name);
    public string cookie(string name);          // 按RFC 6265 §5.4取首个同名；缺→""
    public bool has(string key);
    public Object get(string key, Object def = null);
    public string getString(string key, string def = "");
    public int getInt(string key, int def = 0);
    public bool getBool(string key, bool def = false);
    public void set(string key, Object value);
    public void remove(string key);
    public Session session();                   // 惰性建会话；有写入才发Set-Cookie（D-12）
    public void setCookie(string name, string value, string path = "/", int maxAgeSeconds = 0,
                          bool httpOnly = true, bool secure = false, string sameSite = "Lax");
                          // maxAgeSeconds三态：0不发Max-Age（会话级）、正数发Max-Age: n、
                          // 负数发Max-Age: 0（立即过期，删除用）——映射见D-12
    public void text(int status, string body);  // text/plain; charset=utf-8
    public void html(int status, string body);  // text/html; charset=utf-8
    public void json(int status, json.JsonValue value);   // 紧凑序列化
    public void bytes(int status, byte[] body, string contentType);
    public void redirect(string location, int status = 302);
    public void noContent();                    // 204且清正文
    public void next();                         // 推进洋葱；不调即短路（D-3/D-4）
}
```

### 6.4 `Middleware`

```
class Middleware {
    public virtual void process(RequestContext ctx);   // 基类默认实现 = ctx.next()
}
```

子类可在构造器接收配置、持有字段（跨请求共享，须无状态，D-25）；无自有构造器的子类直接`new`。

### 6.5 `Router`

```
class Router {
    public void on(string method, string pattern, func<void, RequestContext> h);
    public void get(string pattern, func<void, RequestContext> h);   // 同App的简写族
    public void post(string pattern, func<void, RequestContext> h);
    public void put(string pattern, func<void, RequestContext> h);
    public void patch(string pattern, func<void, RequestContext> h);
    public void del(string pattern, func<void, RequestContext> h);
    public void head(string pattern, func<void, RequestContext> h);
    public void any(string pattern, func<void, RequestContext> h);
    public void mount(string prefix, func<void, RequestContext> h);
    public void mount(string prefix, Router r);            // 子路由分组（D-10）
    public bool dispatch(RequestContext ctx);          // 命中则跑handler并返true；未命中false且不改res
    public void serve(RequestContext ctx);             // dispatch的void包装：未命中写本子树的404（方法不匹配写405带子树`Allow`），供mount挂载（D-10）
    public string allowFor(string path);               // 405的Allow串；无其他方法命中→""
}
```

`App`持有一个`Router`并转发注册。全表未命中（挂载条目也算条目）时的404与405由`App.handle`写（D-8/D-11）；挂进子树的`Router`经`serve`写**本子树**的404与405（D-10），所以同一个`Router`既能独立复用也能嵌套，而响应只会被写一次——`dispatch`未命中时不改`res`，写响应的只有`serve`与顶层`App.handle`两条路。

### 6.6 会话

```
class Session {
    public string id;
    public bool isNew;
    public bool isExpired;
    public long expiresAtMs;                  // monoMs域（D-12）
    public bool has(string key);
    public Object get(string key, Object def = null);
    public string getString(string key, string def = "");
    public int getInt(string key, int def = 0);
    public void set(string key, Object value);
    public void remove(string key);
    public void destroy();                    // 清服务端记录 + 让本次cookie立即过期（发Max-Age: 0，D-12）
    public void rotate();                     // 换id，登录成功后必调（D-12）
}
class SessionStore {
    public virtual Session load(string id);   // 无记录或已过期→null
    public virtual void save(Session s);
    public virtual void destroy(string id);
}
class MemorySessionStore : SessionStore {
    public MemorySessionStore(int idleTtlMs = DEFAULT_SESSION_TTL_MS,
                              int sweepIntervalMs = DEFAULT_SESSION_SWEEP_MS);   // 顺带清扫的最小间隔，无后台线程（D-12）
    public int sweepExpired(long nowMs);      // 按传入monoMs清扫，返清除数（确定性可测）
}
```

### 6.7 `StaticFiles`（模块`web.static`）

```
class StaticFiles {
    public StaticFiles(string root);          // root不存在或非目录→具名Exception（D-24）
    public string index = "index.html";
    public int maxAgeSeconds = 0;             // 0 = 不发Cache-Control（D-14）
    public bool useETag = true;
    public bool allowDotFiles = false;        // 以.起始的段默认按不存在处理（D-14）
    public void addType(string ext, string mime);
    public void serve(RequestContext ctx);    // 301/304/404/405（405带`Allow`）由本方法置res（D-14）
}
```

典型挂法：`app.mount("/static", assets.serve);`——函数引用指向绑定方法，与`App`内部分派同形（绑定方法作函数引用实参已由探针X4实测，见3.2的P2行）。根挂载`app.mount("/", assets.serve)`同样合法，但它是全表最宽的前缀条目，因此要放在所有API路由**之后**登记（D-9/D-10）；ACME一类的`/.well-known`需要`allowDotFiles = true`。

### 6.8 `Sse`（模块`web.sse`）

```
class Sse {
    public Sse(http.HttpResponse res);        // 置事件流头 + 接bodyChannel
    public void send(string data);
    public void send(string event, string data);
    public void send(string id, string event, string data);
    public void comment(string text);         // 心跳注释，防中间设备掐连
    public void close();
}
```

### 6.9 表单与上传（模块`web.form`）

```
class Form {
    public static FormData parse(RequestContext ctx);                    // 用缺省上限（D-18）
    public static FormData parse(RequestContext ctx, FormOptions opts);  // opts为null同缺省
}
class FormOptions {                     // 公开字段，new出来改完再传（D-18）
    public long maxBytes = DEFAULT_MAX_FORM_BYTES;        // 8 MiB
    public int maxParts = DEFAULT_MAX_FORM_PARTS;         // 1000
    public int maxPartBytes = DEFAULT_MAX_FORM_PART_BYTES; // 4 MiB，字段与文件共用
}
class FormData {                        // 保序多值，同名不合并（D-19）
    public List<string> names();              // 字段与文件名，按出现序去重
    public bool has(string name);
    public string get(string name);           // 首个值；缺→""
    public List<string> getAll(string name);  // 同名多值按出现序
    public FormFile getFile(string name);     // 首个同名文件；缺→null
    public List<FormFile> getFiles(string name);
    public List<FormFile> files();            // 全部文件，按出现序
    public int fieldCount();                  // 非文件部件数
    public int fileCount();                   // 文件部件数（同名各计一个）
    public int partCount();                   // 全部部件数（`names()`去重，计数不去重）
}
class FormFile {                        // 非const类：含byte[]字段（探针P6）
    public string field;                      // name参数值
    public string filename;                   // 剥目录路径后的原样值，可为空串（D-19、D-20）
    public string contentType;                // 缺省登记为"text/plain"（D-19）
    public byte[] data;                       // 正文，整段正文的视图（D-18）
    public int size();
    public string text();                     // 按UTF-8解码
    public string safeName();                 // 清洗名（D-20）
    public void saveTo(string dir);           // safeName + 随机后缀落盘
    public void saveAs(string path);          // 原样落盘，调用方自负清洗
}
class MultipartBody {
    public MultipartBody();                   // boundary = 20字节随机数转40位hex（D-21）
    public void addField(string name, string value);   // 不带Content-Type、不带filename
    public void addFile(string field, string filename, string contentType, byte[] data);
                                              // contentType为空→"application/octet-stream"
    public string contentType();              // multipart/form-data; boundary=...
    public byte[] toBytes();                  // 逐部件CRLF纪律 + boundary冲突自检
}
```

### 6.10 承建面变更

```
// http：优雅关闭（D-22）
class HttpServer {
    public int drain(int graceMs = DEFAULT_DRAIN_GRACE_MS);         // 排空，返残余连接数
    public void close();                                             // 语义改为强关
    public bool isServing();
}
// net：EOF判别（D-23；纯源码读既有粘滞标志m_winEof，无宿主改动）
class TcpStream { public bool isEof(); }
// fs：写与mtime、读返回类型（D-15）
native byte[] readBytes(string path);         // 原 int[]
native void writeBytes(string path, byte[] data);
native long modifiedTimeMs(string path);
// math：CSPRNG字节（D-13）
native byte[] randomBytes(int count);
```

### 6.11 用法示例

```
import io;
import system;
import http;
import web.*;

void auth(web.RequestContext ctx) {
    if (ctx.req.headers.has("X-Key") == false) { ctx.text(401, "missing key"); return; }
    ctx.next();
}

class Timing : web.Middleware {
    public void process(web.RequestContext ctx) {
        long t0 = system.monoMs();
        ctx.next();
        ctx.res.headers.set("X-Elapsed-Ms", (system.monoMs() - t0).toString());
    }
}

void showItem(web.RequestContext ctx) {
    ctx.text(200, "item " + ctx.param("id"));
}

void upload(web.RequestContext ctx) {
    web.form.FormData form = web.form.Form.parse(ctx);
    web.form.FormFile f = form.getFile("file");
    if (f == null) { ctx.text(400, "no file"); return; }
    f.saveTo("uploads");
    ctx.text(200, "saved " + f.safeName() + " " + f.size().toString());
}

void login(web.RequestContext ctx) {
    web.Session s = ctx.session();
    s.set("user", "dlite");
    s.rotate();                       // 权限跃迁必换id
    ctx.text(200, "ok");
}

int main() {
    web.App app = web.App();
    app.use(Timing());
    app.use(auth);
    app.get("/items/:id", showItem);
    app.post("/login", login);
    app.post("/upload", upload);
    web.static.StaticFiles assets = web.static.StaticFiles("public");
    app.mount("/static", assets.serve);
    app.listen("127.0.0.1", 0);     // 端口0=临时端口；起服务后本线程接着往下跑
    string base = "http://127.0.0.1:" + app.server().localPort().toString();
    io.print(http.get(base + "/items/7").bodyText());   // item 7
    io.print(app.drain(5000).toString());               // 排空在途请求，返残余连接数（D-22）
    app.close();
    return 0;
}
```

`serve`是独立服务进程的入口（`main()`不得返回，与`stdlib/http.n:1551-1557`的注释同调），`listen`给「起服务后本线程接着干活」的场景（测试与嵌入）。发布的`examples/web_hello.n`按在树示例的一致姿态自终止：`listen`起服务、自带客户端跑几趟请求、再`drain`后退出，绝不死循环（ctest对`.n`示例只做编译校验（带`.nproj`运行配置的`mnist_mlp`除外），但一个跑起来不退出、只能靠超时收场的示例同样没人想看）。

## 七、测试策略

用真实测试：编译器真编译`.n`、VM真跑字节码、TCP真往返（既有测试纪律）。新增e2e条目入`tests/e2e/manifest.txt`：

| 条目 | 钉住什么 |
| --- | --- |
| `stdlib_web_routes_matrix` | 精确优先、`:id`捕获与解码、`*`尾通配、段数不符不配、405带`Allow`、404、HEAD复用GET、注册冲突与非法模式的具名拒绝；挂载面：前缀命中即让渡子树（不回落）、请求路径恰等于前缀时余下`"/"`、子树未命中由被挂载组件写404/405、同层遮蔽与死条目的注册期具名拒绝（`mount("/api")`在先时后登记的`get("/api")`放行、`get("/api/:id")`拒、`mount("/api/v2")`拒，`any("/*")`在先时后登记的`mount("/api")`拒；`mount("/")`合法且其后的参数化条目被拒） |
| `stdlib_web_middleware_styles` | 两风格混排的洋葱次序、短路、后置回程、异常穿透、每请求状态隔离、`next()`重入→500具名（探针P5固化为语料） |
| `stdlib_web_session_basic` | cookie往返、无写入不发`Set-Cookie`、`Set-Cookie`属性三态（会话cookie与默认`setCookie`都不带`Max-Age`、传正数带`Max-Age: n`、`destroy()`之后为`Max-Age: 0`，D-12）、`rotate()`后旧id失效、`destroy()`、`sweepExpired(expiresAtMs + 1)`的确定性过期 |
| `stdlib_web_session_conc` | 多id并发百次压测（无时间断言；last-write-wins语义按手册钉死） |
| `stdlib_web_static_files` | index回退、扩展名表、穿越各形态→404（`..`、`.`、NUL、'/'起始、盘符起始、根化，外加解码后段值内含分隔符的`x%2F..%2Fx`与`x%5C..%5Cx`两形态）、点文件默认404且`allowDotFiles = true`后按普通文件服务、目录未带尾斜杠→301且`Location`＝原路径＋'/'而query原样、`text/*`带`; charset=utf-8`且非文本类型不带、弱ETag与304、`If-Modified-Since`秒粒度、HEAD只发头、未知扩展名→octet-stream、非GET/HEAD→405且带`Allow: GET, HEAD`、根目录不合法→构造期具名拒绝 |
| `stdlib_web_sse_frames` | 逐字节帧对拍：多行data、`event:`、`id:`、注释行、`close()`后的分帧终止；响应头集合钉死：`text/event-stream`＋`Cache-Control: no-cache`＋`Connection: keep-alive`＋`X-Accel-Buffering: no`＋分块传输且不带`Content-Encoding`（后者由`stdlib/http.n:1209`那条四条合取的gzip门槛保证——`Sse`既不开`res.compressBody`也不填`res.body`，此腿是防回归钉，D-16）；帧的到达时刻不作断言（无时间断言裁定），只钉帧序与字节 |
| `stdlib_web_form_urlencoded` | 切分、'+'→空格、百分号解码、同名多值序、空值合法、非表单Content-Type→415、带参数的`application/x-www-form-urlencoded; charset=utf-8`仍走urlencoded分支 |
| `stdlib_web_multipart_parse` | 宽容矩阵：quoted boundary、无前导CRLF的首定界、preamble、`\n`与`\r`换行、定界行行尾水平空白、闭合线后空格/尾随CRLF/epilogue、obs-fold并入值、70字符boundary、正文含`--boundary-`前缀行（即`--foobar`不等于`--foo`）；语义面：`filename=""`算文件、无Content-Type部件登记`text/plain`、其余部件头被忽略且`Content-Transfer-Encoding`不解码、嵌套部件当不透明字节、`filename`不被percent解码、`filename`的目录前缀被剥掉（`C:\fakepath\x.png`→`x.png`、`a/b/c.txt`→`c.txt`、`..`与百分号原样留着）、同名部件保序不合并、0x00与0xFF直通 |
| `stdlib_web_multipart_reject` | 400矩阵：缺`boundary`参数、多个`boundary`参数、缺`Content-Disposition`、缺`name`与空`name`、无闭合定界线、部件头超16 KiB、字段名超100字节；每例断言消息具名且不是415 |
| `stdlib_web_multipart_limits` | `maxBytes`/`maxParts`/`maxPartBytes`三道越限→413且消息具名撞上哪一道与限额值 |
| `stdlib_web_multipart_roundtrip` | `MultipartBody`→`Form.parse`往返恒等（含中文文件名与二进制部件）＋boundary冲突自检生效 |
| `stdlib_web_upload_e2e` | 客户端上传→服务端解析→`saveTo`落盘→`fs.readBytes`读回按字节对拍（`fs.writeBytes`新面） |
| `stdlib_http_drain_close` | 排空返0、超时强关返残余、**停在keep-alive等待里的空闲连接被主动关闭**（不等自己的`keepAliveTimeoutMs`；关后该连接线程随即收尾——登记表清空、`isServing()`为假，D-22的parked唤醒路径由此得证，无需时间断言）、排空期写出的响应一律带`Connection: close`、超出`maxConnections`而被拒的连接不留登记表项（把上限设为1：唯一那条连接停在处理器内持住在途请求，随后一条连接撞容量被拒；宽限给一个够不着的量→走超时强关腿→`drain`返**恰好1**。残余数按定义是宽限到期时仍在登记表内的连接数，而被拒连接既不`serveOne`也无连接线程去注销，登记若落在`m_active`自增处（`:1582-1583`），那条永不注销的表项会把残余抬到2——这一腿因此区分两个登记点；「压满上限后返0」写法证不出任何东西，因为残余只在超时腿产生、被拒连接又从不碰`m_inFlight`）、排空期正在推流的SSE连接不在立即强关集合内（宽限内写完剩余帧再收尾，D-22的窗口口径由此得证）、`close()`后端口可重绑、复位后重新`start()`可服务、双`start()`拒绝、`drain`/`close`重复调用幂等（已closed再`drain`立即返0）。排空腿一律以「先建立、再断言」的因果序取代等待（无时间断言，既有裁定）：客户端发完请求即持住连接、服务端`drain`的宽限给一个够不着的量 |
| `stdlib_net_eof_contrast` | `read`/`readLine`/`readExactly`×对端EOF/超时矩阵＋`isEof()`与`isClosed()`正交 |

C++侧单元测试三枚（native）：`math.randomBytes`两次调用不同且非全零；`fs.writeBytes`/`readBytes`字节往返；`fs.modifiedTimeMs`对新建文件不早于写入时刻（单调性断言，不做绝对时间断言）。`isEof()`属源码面，其与`isClosed()`的正交性由e2e的`stdlib_net_eof_contrast`钉住（本地先关／对端先关／双向／都活四象限）。

门禁与口径：三树（Debug断言＋ASan＋IDE）全量ctest经`tools/gate.sh`，只认判决行；预置包与编译器变更必须跑IDE树；e2e全套串行；三树e2e各自确认native DLL与asan运行库拷贝到位（陈旧拷贝纪律）；慢测试（>30s）只在提交前批跑。任一门禁失败修复后该批全部结果作废重扫。

性能：本版不改http热路径，只在处理器前多一层`App.handle`；以`examples/http_conc_bench.n`形态做同机对拍冒烟，确认无数量级回退（v0.13.6的性能门槛不在本版重判）。

## 八、集成点与文档同步

新增文件：包级模块`stdlib/web.n`与子模块`stdlib/web/static.n`、`stdlib/web/sse.n`、`stdlib/web/form.n`（D-28）、`examples/web_hello.n`、`examples/web_upload.n`、`docs/user_manual/en/specification/library/27-web.md`与zh同名页、上表14枚e2e。

修改文件：
- `stdlib/http.n`（三态＋登记表＋`HttpConn.forceClose()`＋`drain`/`isServing`＋`close`改强关＋复位与双启动拒绝）；
- `stdlib/net.n`（新增`isEof()`读既有粘滞标志，宿主与POSIX侧均无改动）；
- `stdlib/fs.n`＋`src/native/fs/fs_native.cpp`（`writeBytes`/`modifiedTimeMs`/`readBytes`返回类型）；
- `stdlib/math.n`＋`src/native/math/math_native.cpp`（`randomBytes`）；
- `CMakeLists.txt`（预置包收集改递归：`file(GLOB …)`改`GLOB_RECURSIVE`、生成的`.nproj`按相对路径写`<File path="web/static.n"/>`、`nlang_stdlib_package`的拷贝改成保留目录结构的形态——按文件名的`copy_if_different`会把子目录压平；`install(DIRECTORY stdlib/ …)`本就递归）；
- `tools/test_families.txt`（`stdlib_web`族的`src`规则补一条`stdlib/web/*.n`，否则子模块的改动落到未映射路径、T0按保守口径回退全量Debug单测）；
- `tests/e2e/manifest.txt`、`examples/mnist_mlp/data.n`与`tests/e2e/fs_read_bytes_floats.n`（随`readBytes`变更）；
- `tests/test_compiler/test_library_index.cpp`与`tests/test_langservice/test_symbol_index.cpp`（两处`kStdlibSymbolPin`＝336是预置包公开符号总数钉值，`web`包与三枚新native落地后必随实际值刷新）；
- `docs/dev/stdlib_public_surface.md`（预置包公开面清单）、`README.md`与`README.zh-CN.md`（标准库清单里的包列表）；
- 手册`library/13-math.md`、`library/16-fs.md`、`library/20-library-exceptions.md`（三枚新native与`readBytes`变更的异常行）、`library/21-network/tcp.md`、`library/23-http.md`，formats三页27/28/29→28/29/30（中英成对），`index.md`与导航/搜索索引覆盖新页；
- `src/compiler/builder/ExprResolverStdLib.cpp`、`src/compiler/builder/ModuleRegistryGate.cpp`与`src/compiler/ModuleBuilderImports.cpp`（D-29：限定名解析统一到最长模块路径优先、`import p.*;`覆盖库包子树、诊断只对模块路径说未导入）＋`tests/test_compiler/test_module_import.cpp`（改写`builtinWildcardRejected`那枚通配拒绝断言，补限定成员与库包子树的钉值）＋`tests/e2e/`新语料与`tests/e2e/manifest.txt`；
- `docs/user_manual/{en,zh}/specification/03-program-structure/index.md`（§3.3的库包通配适用范围与§3.6的限定名成员访问，中英成对）与`docs/user_manual/{en,zh}/specification/04-types/class.md`§4.8.8（静态成员的两种访问拼写，中英成对）；
- `docs/dev/name_resolution_design.md`（§5的V2/V3加带日期修订：V3「库包禁通配」随D-29规则2撤销，V2的限定名准入按D-29规则1扩到类型成员）；
- `VERSION`（0.13.11→0.13.12）、`CHANGELOG.md`与`CHANGELOG.zh-CN.md`、`docs/roadmap.md`（本版条目补写multipart与上传面；「远期特性」的语言层面缺口清单**逐项**登记九.18的四条缺口——带默认值的func形参拒绝函数引用、实例构造调用的实参位置拒绝函数引用、下标元素不能就地调用、`Dict`缺`getOrDefault`——各条附本文第九章指针，而不是只给一条指向第九章的总指针）。

手册同步是本版完成定义的一部分（`tools/manual_sync_check.py`基线以来的提交逐个判定，五类用户可见面）：标准库面（`web`新包全部签名、`fs`三处、`math.randomBytes`、`net.isEof`、`http`服务器关闭语义与状态）、诊断文案（415/413/400具名消息、路由冲突文案、双启动文案，另D-29移除`Wildcard import cannot target library package`并把误报的`Module '…' is not imported`收窄到真正的模块路径）、退出码（无变更）、ABI与线格式（无变更）、语言层面（D-29的两处名称解析修复：库包通配导入与经限定名的类静态成员访问；D-27的四条缺口只进roadmap登记）。用户可见的类都有`.n`源码（`stdlib/web.n`与三枚子模块）满足要求；手册`27-web.md`按模块分列公开面（`web`、`web.static`、`web.sse`、`web.form`），并写明引用子模块的类型需`import web.static;`一类或`import web.*;`——只`import web;`时点号子模块不在名册里（D-28）。

## 九、后续版本备忘

以下是本版不做项与递延登记的备忘：功能项的内容与递延理由已在2.2各列一行，这里给将来实现时的落点与设计要点；18（本版登记的语言层面缺口）、19与23（命名与公开面整理）属登记项，不在2.2列出。每项将来成版时按既定流程走自己的概要设计，本节只负责不丢信息。

1. **WebSocket**：需要`Upgrade: websocket`握手（`Sec-WebSocket-Accept`=SHA-1(key+GUID)的base64，SHA-1需新native）、掩码与帧型（文本/二进制/分片/ping/pong/close）、以及分帧后的读写与http层的复用关系。参考：RFC 6455 §5.2与§1.3、Go`golang.org/x/net/websocket`与`gorilla/websocket`。落点建议：`web.WebSocket`＋`app.get("/ws", ...)`内的`ws.accept(ctx)`，握手后的流走`net`而非http层，因此需要http层交出裸流（现状`HttpConn`不外露）。
2. **模板引擎**：语法选型（Mustache式`{{}}`／Go`text/template`式动作／`nlang`自有）一旦定型改动代价大；本版由`ctx.html`＋字符串插值＋`json`覆盖主路。参考：Go`text/template`、Jinja2、Handlebars。落点建议：独立包`template.n`，与`web`的接口只是`render(name, value): string`。
3. **流式上传落盘**：设计要点是visitor形态的解析入口（`onPartHeader`/`onPartData`/`onPartEnd`三虚方法的基类，与中间件同风格）、`fs.appendBytes`、临时文件命名与请求结束后的清理（含异常路径）、以及「字段与文件混排时字段可能晚于文件到达」的顺序陷阱。参考：Go`mime/multipart.Reader.NextPart`直读＋`ReadForm`的spill逻辑、Werkzeug spooling、multer `diskStorage`。
4. **multipart嵌套部件**：RFC 2046允许多部分嵌多部分，RFC 7578表单场景禁；真有消费者时按「递归解析＋深度上限」实现，复用D-18扫描器。
5. **路由级中间件与分组**：闭包落地（v0.13.10）后，`func(http.Handler) http.Handler`式或`route.use(m)`式都可直接表达；届时把D-3的链改成「链的构造期在路由命中时」，`RequestContext`无需变。
6. **外置会话存储与分布式**：`SessionStore`虚基类已为此留口（D-12）。外置存储要点：序列化格式（`json`可用）、TTL交给后端（Redis `EXPIRE`）、集群下的粘性或集中存储、以及每会话锁（现有last-write-wins语义届时重新裁决）。参考：express-session store接口、Gorilla `sessions`的`Store`接口。
7. **签名或加密cookie**：无状态会话需HMAC-SHA256与AES-GCM原语（新增native），以及轮换secret的验签窗口（express-session的secret数组语义：首元素签、全部验）。参考：express-session、Rack session cookie。
8. **CSRF／限流／日志／recovery中间件**：都是框架之上的策略件。CSRF需双提交cookie或同步令牌＋`SameSite`关系说明；限流要点：进程内令牌桶＋虚拟线程调度；recovery即`onError`的默认实现变体。参考：django CSRF、express-rate-limit、Gin recovery。
9. **响应压缩中间件**：`gzip.compress`/`decompress`已有（v0.13.5），`http`层的**线上半**也已有——`writeResponse`的门槛是`!noBody && res.compressBody && body != null && acceptsGzip(req)`（`stdlib/http.n:1209`），命中即压并置`Content-Encoding: gzip`与`Vary: Accept-Encoding`（`:1210-1212`），而`Content-Length`在压缩之后才写（`:1230-1231`），三者顺序与一致性不必中间件操心；`Vary`只在真压时带也站得住——RFC 9110 §12.5.5把该字段定为SHOULD且明确允许为缓存性能省略，漏带的方向（identity条目被复用给接受gzip的客户端）只是少压一次，会出错的方向（gzip条目被复用给不接受的客户端）已被压缩那一侧的`Vary`挡住。缺的是**决策半**：按响应的Content-Type与正文大小决定要不要把`res.compressBody`置真（已是压缩类型与图片再压一遍纯烧CPU、过小正文不值得压），以及策略自身依赖的请求维度要不要一并进`Vary`（`http`层只申报`Accept-Encoding`一条）。还含中间件对已流式（`bodyChannel`）响应的处置：那条门槛要求`body != null`，故流式响应不经压缩已是既成事实（D-16），届时要写明「见到`bodyChannel`就不碰」而不是替它另开一条压缩路，还要处理把`body`与`bodyChannel`同时填满的混用形态（`stdlib/http.n`今天会给它盖上`Content-Encoding`而通道字节原样泵出，D-16）。与静态资源的条件请求天然相容：静态面的ETag本就是弱式，一个弱ETag可同时合法服务gzip与identity两个变体（RFC 9110 §8.8.3.3注、§13.1.2，理由见D-14）。参考：compression的`filter`与`threshold`（默认按Content-Type可否压缩与正文大小决定）。
10. **反向代理头信任**：`X-Forwarded-Proto`/`For`/`Host`只在明确信任代理时可用（express的`trust proxy`、ASP.NET Core的`ForwardedHeadersMiddleware`），否则客户端可自造头污染`Secure`cookie判定与日志来源。落点建议：`App`的代理信任配置＋`ctx.remoteAddress`/`ctx.requestUrl`的正确来源判定。
11. **Range请求**：206单段、416、多段`multipart/byteranges`三件套，与ETag/`If-Range`纠缠。参考：Go`fs.go`的`serveContent`、RFC 9110 §14.2与§15.5.17。
12. **目录列表**：与静态服务的缓存与`index`语义纠缠（列出的条目要不要ETag、排序、`Order`指令）；`fs.listFiles`已给非递归名单。参考：nginx`autoindex`、Go`http.Dir`的`List`。
13. **HTTP/2**：分帧层＋HPACK＋流控＋服务端推送取舍，与现有「每连接一处理器线程」模型需重新设计。参考：RFC 9113、hyper/h2。
14. **表单绑定到struct**：把`FormData`按字段名灌进struct，依赖v0.13.9的`&`形参（避免整记录拷贝）与按字段名的赋值能力；校验错误聚合也要一并设计。
15. **闭包落地后的写法收敛**：roadmap已记「v0.13.10闭包落地后再替换」。届时`Middleware`子类风格保留（配置与状态场景），函数风格可由闭包表达；注册方法名已经收敛成单名重载（`use`与`mount`各两候选，用户裁定2026-11-17），闭包落地只是让写法更省事，不再欠这项收尾。
16. **下载响应头的`filename*`**：上传解析不认`filename*`（RFC 7578 §4.2 NOTE「The encoding method described in [RFC5987] … MUST NOT be used」、MDN「the request header does not have the filename* parameter」），但**响应侧**的文件下载头正是它的用武之地：RFC 6266 §4.3要求`filename`与`filename*`同时在场时收件人优先取后者。届时要做`contentDisposition(filename, kind)`一类的生成器，并处理`UTF-8''…`的charset与`lang`段、未知charset、RFC 8187的扩展参数写法，以及`filename`回退值的清洗（RFC 6266 §4.3点名控制字符、首尾空白、`.`/`..`、`~`、`|`与设备名）。清洗规则可与`FormFile.safeName()`同源。参考：RFC 5987/6266/8187、MDN`Content-Disposition`。
17. **会话并发与过期策略**：并发语义现状是last-write-wins（D-25）。将来若要「同会话请求串行」，落点是store的per-id信号量＋等待上限，并须重新裁决死锁与长任务（SSE会话）的关系；过期策略现状只有空闲TTL（D-12），绝对上限与到期滚动刷新（express-session的`cookie.maxAge`配`rolling`）应当一并设计，否则`expiresAtMs`会被两套时限拉扯。参考：Rails`ActiveRecord::SessionStore`教训、express-session文档对并发请求与`rolling`的说明。
18. **本版登记的语言层面缺口**（进roadmap「远期特性」语言层面清单，不记入手册已知限制）：带默认值的func形参上下文中函数引用被拒（P2，探针F3；修法是把期望函数的解析延后到实参形态已知，或在期望类型解析里带上形参的声明类型）；实例构造调用的实参位置同样不给函数引用预期类型（`FuncMiddleware(handler)`一类`Type(args)`写法，自由函数引用与绑定方法引用都被拒，同一枚引用在普通调用实参、字段赋值与带类型的局部声明三处都能绑定，缺口只在构造调用那一格，探针`temp/t6_probe3.n`实测2026-11-19；与第一条同属期望类型的传播缺口，一条修法覆盖两者）；容器元素不能就地调用（`mws[0](ctx)`一类一律语法错，必须先落进同类型局部变量，P7，探针W1/W2/W4/W6/X6b；影响所有「把函数引用装进容器」的写法，不止本框架）；`Dict`缺`getOrDefault`而`get`缺键抛异常（P4，配套`Set.contains`风格一致性核）。同日实测的另两处名称解析缺口不入本条：限定名的类静态成员与库包通配导入按D-29当版改正编译器，既不递延也不记入手册已知限制。
19. **`http`面的动词整理**：`HttpClient.open(req)`的`open`会被读成「打开连接」，实际义为「发起流式交换」（连接由池透明借还，缓冲版叫`request()`），若清理该面可考虑改名`stream(req)`；同批一并处理两个`drain`的分工（`HttpBodyReader.drain(int)`吃残余正文、`HttpServer.drain(graceMs)`排空在途请求，现以类名限定语区分）。两者都只动命名不动语义，代价是手册§23.4/§23.5/§23.6与全部在树示例跟改，与本版功能正交，故递延。
20. **部件头的三类历史解码**：`Content-Transfer-Encoding`（quoted-printable与base64；RFC 7578 §4.7已请发送方不要再发、「no deployed implementations that send such bodies have been discovered」，唯Go`NextPart`特判解码而`NextRawPart`不解）、RFC 2047 encoded-word的字段名与值（§5.1自述「this practice doesn't seem to have been followed widely」，Werkzeug/busboy/Rack都不解）、RFC 2231的续行与扩展参数（`name*0*=…`，Go按「invalid parameter value」拒）。本版一律不透明直通。主要候选来源是邮件网关转发的表单；真要做时解码层加在D-19读完部件头之后，并必须先定下唯一口径，避免「同一个body经不同API拿到不同字节」。
21. **文本部件的charset梯度**：RFC 7578 §5.1.2给的梯度是部件`Content-Type: text/plain; charset=…`＞`_charset_`字段（§4.6）＞文档编码＞UTF-8，服务的是HTML form遗留场景；本版按WHATWG fetch固定UTF-8。将来落地要一并裁决`charset=`值不在白名单内的处置（Werkzeug的白名单是`ascii`/`us-ascii`/`utf-8`/`iso-8859-1`并自注不再扩充）、`_charset_`字段的全局生效范围，以及`FormData.get()`与`FormFile.text()`两条解码路径同源。
22. **`a[b][c]=`参数树**：Rack与PHP会把扁平字段名重建成嵌套结构，那是应用层的表单语义；本版`FormData`只交保序的(name, value)序列，信息无损失。真要做须连带命名文法、数组下标、与既有同名标量的冲突判定，并同九.14的表单绑定struct一起设计。
23. **media type参数解析器公开化**：D-17要在`web.n`内私有一枚「类型名＋参数表（含quoted-string去引号）」的解析器，`HttpClient`读响应类型、内容协商、下载头生成都用得上同一件东西。届时提升为`http`面的公开函数或`MediaType`小值类，须同时补手册§23的相应小节与`.n`源码面；提升后`web`侧改为调用，避免两处各存一份文法。

## 十、修订记录

- 2026-11-17 初版。纳入用户三项裁定：中间件须同时支持独立函数与子类虚方法两风格（D-3）；multipart/form-data纳入本版（D-17至D-21）；本版不做项一律登记为第九章备忘。勘察与探针结论见第三章（探针在`temp/web_probe/`），外部取证见第四章与`temp/ref/`两份调研。
- 2026-11-17 自审修订＋关闭面命名裁定（用户提议改叫`drain`/`terminate`）：排空入口定名`drain(graceMs)`、释放仍叫`close()`（放弃`shutdown`、未采`terminate`，理由见D-22），`web`层不新增打开动词（3.1补两条动词惯例实证），两入口补幂等条款；另补`Router.serve`挂载形态（D-10）、会话改惰性清扫（D-12）、`StaticFiles`根目录构造期校验（D-24）、可变store与`FormFile`的`const`取舍（探针C1/P6）、手册同步面补`20-library-exceptions.md`与`21-network/tcp.md`；九.17补会话绝对上限与滚动刷新、九.19登记`HttpClient.open()`改名候选。
- 2026-11-17 multipart取证回填（`temp/ref/multipart_research.md`核对，纠正初版凭印象写的条目）：请求侧`filename*`按RFC 7578 §4.2的「MUST NOT be used」移出解析面、优先级归到下载响应侧（九.16改写）；第四节busboy缺省值改为实测值（`fieldSize` 1 MiB、`fieldNameSize` 100、`fields`/`files`/`parts`均Infinity、部件头硬编码16 KiB＋2000对，并无`headerFieldSize`这项），部件头上限随之由8 KiB改16 KiB；「Django`secure_filename`」更正为Werkzeug（Django是更弱的`get_valid_filename`）；obs-fold由拒绝改并入值（RFC 7230 §3.2.4的第二选项，Werkzeug与Go都取此）；415的RFC条款由§15.5.13更正为§15.5.16。补齐此前缺失的文法条目：首定界行可无前导CRLF、`\n`与`\r`逐处容忍、定界行按后缀字符分类匹配（`--foobar`不等于`--foo`）、quoted边界与行尾水平空白、多个`boundary`参数→400、缺`Content-Disposition`→400、`filename=""`算文件、其余部件头必须忽略、`filename`不自动percent解码、同名部件保序不合并。上限由四道改三道并重定缺省（整段8 MiB／部件数1000／单部件4 MiB，去掉`maxFileBytes`），并点名标准库缺media type参数解析器这一缺口（D-17私有helper＋九.23）；构造侧补boundary只取十六进制、逐部件CRLF纪律、参数值转义取HTML子集与文本部件不带Content-Type。另记浏览器侧`multipart/form-data`解析无规范标准（fetch自认「rough approximation … to be written」），据此把「两家以上主流实现一致」定为D-19宽容清单的判据；九.20至九.23与2.2的六行同步登记，e2e拆出`stdlib_web_multipart_reject`（共14枚）。
- 2026-11-17 第二轮复审落实（11项意见＋探针复跑新发现，一次性改完）：**分派确定性**（F1）——挂载条目以前缀参与同一张注册序表、前缀命中即让渡子树（不回落、不必还原`ctx.path`）、子树404/405由被挂载组件自己写、后登记路由整条落在先登记前缀之下改为**注册期拒绝**（D-7/D-9/D-10）；**优雅关闭接线**（F2）——排空门槛从按连接计数的`m_active`换成按请求计数的新计数`m_inFlight`（`m_active`只用于`maxConnections`），补上Go`Shutdown`常被漏掉的两半（排空期一律盖`Connection: close`、主动强关停在keep-alive等待的空闲连接），已closed再`drain`立即返0，`DEFAULT_DRAIN_GRACE_MS`＝15000ms给出取法（D-22）；**函数引用规则更正**（F3＋用户裁定「合并成单名重载」）——实测重载并不阻挡函数引用（探针X1–X4），被拒的只有带默认值的func形参（探针F3），据此`use`与`useMiddleware`并为单名`use`、`mount(string, Router)`重载恢复（作废D-10原「不加第二个候选」的理由与D-27的两名分裂）；**SSE缓冲义务**（F4）——补`X-Accel-Buffering: no`与逐帧flush，并把它们与压缩跳过判定一起标成调用侧义务（D-16、第四节、九.9）；**路由取向出处**（F6）——改引Go 1.22`ServeMux`的语法子集与「静态＞命名参数＞catch-all」优先级＋注册期冲突`panic`，删掉无出处的chi说法；**multipart两处翻转**（F7）——字段名100字节升为文法级上限（400且具名），`filename`由「解析层不剥路径」翻转为**解析层剥目录路径**（RFC 2183 §2.3、RFC 7578 §4.2、Rack同调），`safeName()`的取末段保留为幂等防御（D-18/D-19/D-20）；**出处补全**（F11）——`close()`的12处声明逐一列出、弱ETag格式标明为本框架自定而非主流同款。另登记探针新发现两条（3.2的P7容器元素不可就地调用、P8`func<void, Ctx>`容器拼写在拼写缺陷修复后成立），入D-27与九.18；自审发现一处内部矛盾已修（第四节说部件头越限报413，与D-19和e2e拒绝矩阵的400冲突）；引用不实两处已改（6.7的绑定方法探针由P5改指X4，`stdlib_web_sse_frames`等e2e行随上述裁定同步）；第七章补挂载、空闲连接、字段名与SSE头的断言腿，第八章补`kStdlibSymbolPin`两处钉值与`docs/dev/stdlib_public_surface.md`、`README*`；6.11示例改成本树示例的一致姿态（临时端口＋自带客户端＋`drain`后退出，补`import http;`）。
- 2026-11-17 第三轮复审落实（3项中级意见一次性改完，另有4项经核实不成立）：**优雅关闭的接线补全**（F1）——补上「谁来关空闲连接」这一半：循环顶部的检查只覆盖已走到请求边界的连接，停在`fill`等待里的连接由`drain`从登记表一侧主动强关，「判为空闲」与`forceClose()`同锁完成，唤醒根据落在native侧的「关者胜出」结算（`src/native/net/net_tcp.cpp:9-16`、`:171-174`；3.1就此新增一行事实）；`Connection: close`的盖章位置钉到`serveOne`既有的`res.connectionClose`汇总行（`stdlib/http.n:1468`，先于`writeResponse`），并交代头为何五条写分支都带、处理器自设的值为何让位、`connectionClose`为何不需要复位点；`m_closing`由`m_state`**取代**而非并存，accept循环退出条件改读`m_state != serving`；排空期的两处判据统一走公开`isServing()`，不新增`isDraining()`（D-22）。**SSE机制措辞按实测改写**（F2）——「逐帧flush」改为引用`pumpChunked`的一元素一次`writeAll`（`:1340-1343`），点明`READALL_WINDOW`（`:679`）与`JOIN_CAP`（`:865`）不在通道路径上，作废不存在的`HttpResponse.writeChunk`与「8 KiB写缓冲」说法；压缩由「跳过判定」改述为**结构性不可能**（`:1209`只压`res.body`），九.9的范围随之收窄（D-16、第四节、九.9）。**e2e两腿**补防回归钉与收尾断言（第七章）。经核实不成立的四项留证于此：`writeChunk`在`stdlib`、`examples`与手册中零出现，故F2所据的API不存在（机制结论仍采纳，措辞已改）；`res.connectionClose`并非「从未被外部写出路径读取」，`writeResponse`在`writeHead`之前据它落头（`:1246-1251`），故F3只作为盖章位置的精确化采纳；「不调`drain`而直接`close`会等满30 s」不成立，`close()`逐个强关登记表内全部连接，代价上界是`DEFAULT_CLOSE_JOIN_MS`＝2000ms（M2）；第四节已把「盖`Connection: close`」与「关掉空闲连接」分列两半（M3），且预置包清单与`tests/e2e/manifest.txt`没有包数量钉值、`stdlib/*.n`由CMake的`GLOB CONFIGURE_DEPENDS`自动收编（M4，`CMakeLists.txt:267-268`）。
- 2026-11-17 第四轮定点核查落实（1项中级＋2项低级＋2项参考级，五项经核全部成立，无驳回项）：**登记位置**（中级）——原写「accept处登记（`stdlib/http.n:1582-1583`）」会把被`maxConnections`拒掉的连接永远留在表里：那两条退路（`:1584-1595`）只回退`m_active`便`continue`，从不进`run()`，也就走不到`connDone()`（`reject503`的`finally`还的是`m_rejecting`，`:1419-1422`），登记表因此只增不减，`drain`的逐个强关与残余数会把死套接字算进去；登记位置改钉在容量分支之后、`thread.spawn(conn.run)`之前（`:1596`），`stdlib_web_drain_close`随之补一枚「压满上限后`drain`残余数为0」的腿（D-22接线点、第七章）。**两处引用挪到实际路径**（低级）——parked唤醒后的抛出点不是`read()`那条`stdlib/net.n:306`，而是空闲等待所在的`fillWindow`（`:424-425`，由`stdlib/http.n:985`的`peekFilled`走到；`net.n:55`的映射只看错误码不看调用名，故各条路径文案一致，关闭后的同方向调用另有`:381`与`:300`守卫）；「关闭推迟到在途操作释放claim之后」补引`net_tcp.cpp:19-21`与机制本体`net_slots.h:5-7`＋`net_slots.cpp:93`（3.1、D-22）。**措辞精度**（参考级）——D-16的「`:1252`写头」改为「`:1252`序列化响应头、`:1261`才把它写出」；「结构上碰不到压缩器」收窄为「gzip分支要求`res.body != null`（`:1208-1209`）而`Sse`不填`res.body`」，并点明两者都填又开`compressBody`的混用形体会给通道响应盖上`Content-Encoding`而字节仍原样泵出，归九.9那条规则处理（D-16、第七章`stdlib_web_sse_frames`行同步）。
- 2026-11-17 第五轮复审落实（两份独立复审合计3项中级＋6项低级或参考级，另加自查的4处引用精度，一次性改完；7项经核实撤销的候选附在最后。第一份报告自称round 2、编号F-R2-1至F-R2-7，与本文第三、四轮的F编号不成同一序列，故按本文修订次序记为第五轮）：**死条目判据重写**（F-R2-1）——原「后登记的路由其静态前缀整条落在先登记的`mount`前缀之下」读不出唯一判据：遮蔽方一侧漏了`mount`条目自身与`any`一类catch-all，等号边界（`get("/api")`遇上`mount("/api")`）无法判读；改以**可达性**定义死条目——先登记条目吃掉后登记的**含参数段或`*`的**条目的全部候选路径即注册期拒绝，判据取两条结构关系之一（`mount`前缀、或catch-all的静态前缀，是后登记条目静态前缀的前缀或与之相等），全静态模式因D-7的精确表优先而豁免，等号边界由此获得独立理由：`mount("/api")`在先时`get("/api")`放行、`get("/api/:id")`拒、`mount("/api/v2")`拒，`any("/*")`在先时后登记的`mount("/api")`拒；代价补一句「兜底路由因此只能作全表末位登记」，`mount`前缀的合法集合同时定死（D-9、第七章`stdlib_web_routes_matrix`行）。**段内编码分隔符**（F-R2-2）——D-7是**先按裸'/'切段、再逐段解码**，故`a%2F..%2Fb`解码成单个段值`a/../b`：老四条判据（`..`、`.`、NUL、'/'起始）全部通过，而`..`被交给文件系统折叠、读取可越过服务根（Windows形态`%5C`同理）；拒绝清单补「解码后的段值内含'/'或'\'」，`stdlib_web_static_files`补这两形态（D-14、第七章）。**`setCookie`的`maxAgeSeconds`三态定案**（F-R2-3）——0＝不发`Max-Age`（会话级，与`StaticFiles.maxAgeSeconds`的「0＝不发`Cache-Control`」同调，同一个数不在同一框架内扛两种含义）、正数发`Max-Age: n`、负数发线上`Max-Age: 0`（立即过期，直接写线上字节而不赌收件人对负数的处置；RFC 6265 §5.2.2对`Max-Age ≤ 0`的处置本就是立即过期，而非钳到0再算，第六轮据此更正了此处措辞；第六轮当时用的「过期时刻＝提取时刻」经自审取`temp/rfc6265.txt`回源再正为该节原话「最早可表示日期时间」（:1091-1092），见D-12）；`Session.destroy()`走负数态，会话cookie自身不带`Max-Age`（绝对上限与滚动刷新仍归九.17），默认值取会话级这个最不意外的结果（D-12、6.3、6.6、第七章`stdlib_web_session_basic`行）。**排空窗口口径**（F-R2-4＋第四轮那条腿作废）——`m_inFlight`的加于`serveOne`之前、减于`serveOne`之后，窗口是整个`serveOne`而非处理器那一段：响应写出（`stdlib/http.n:1469`）与分块泵（`:1261-1262`）都跑在处理器返回之后的同一条连接线程上，按「处理器内」的字面读法会把正在推流的SSE连接判成空闲而主动关闭，排空也可能切在一帧写到一半；按连接那个旗标改名「本次请求处理中」以消除同名两义。**第四轮补的「压满上限后`drain`残余数为0」腿经核实是空断言**：被`maxConnections`拒掉的连接从不进`serveOne`、不碰`m_inFlight`，也没有连接线程去注销它的登记表行，而残余数只在宽限到期那条腿上产生，故两种登记点都能通过这条腿；写法改为「上限设1、唯一那条连接停在处理器内持住在途请求、随后一条撞容量被拒、宽限给一个够不着的量→`drain`返恰好1」，它因此能区分`m_active`自增处（`:1582-1583`）与`thread.spawn`之前（`:1596`）两个登记点，另补「排空期正在推流的SSE连接不在立即强关集合内」的腿（D-22、3.1、第七章）。**静态服务三项补齐**（F-R2-5）——目标为目录且请求路径未带尾斜杠→301且`Location`＝请求路径＋'/'而query原样（Go`fs.Server`、serve-static、httprouter、Werkzeug四方共识，调研注§4坑8；缺它目录页里的相对资源URL全指错处）、表内`text/*`一律带`; charset=utf-8`（调研注§4坑9的历史XSS面，与自家`ctx.text`/`ctx.html`统一姿态）、点文件默认按不存在处理＋`allowDotFiles`开关（serve-static`dotfiles: 'ignore'`缺省，ACME的`/.well-known`需显式开）；'ignore'在本框架落成404而非「让给后面的条目」，因为挂载子树归被挂载组件所有（D-14、6.7、第七章）。**根挂载**（F-R2-6）——`mount("/")`不再被「以'/'结尾即非法」顺带拒掉：它是'/'起始与'/'结尾两条规则的唯一例外，语义为不剥任何字节、余下即请求路径原样，因而是全表最宽的前缀条目、典型位置是最后登记（D-9/D-10、6.7）。**roadmap行粒度**（F-R2-7）——九.18的三条语言层面缺口**逐项**挂进roadmap「远期特性」并各附本文第九章指针，不用一条指向第九章的总指针（第八章）。**压缩引用的自查更正**——D-16的gzip分支条件由「要求`res.body != null`」补全为四条合取`!noBody && res.compressBody && body != null && acceptsGzip(req)`（`stdlib/http.n:1209`），只写一条会读成「非流式就压」；据同一行把2.2与D-16的「缺的是按`Accept-Encoding`择路」更正为缺**决策半**——择路与`Content-Encoding`、`Vary`、`Content-Length`的顺序已由`http`层做完（`:1210-1212`、`:1230-1231`），九.9整条相应改写；新核两处标准依据（原文经`temp/rfc9110.txt`核对，curl取自RFC 9110纯文本）：`Vary`只在真压时带符合§12.5.5（该字段定为SHOULD且明确允许为缓存性能省略；漏带只会让identity条目被复用给接受gzip的客户端而少压一次，会出错的方向已被压缩侧的`Vary`挡住），ETag取弱式的理由补引§8.8.3.3注（内容编码是表示数据的属性，强ETag须在gzip与identity之间不同）与§13.1.2（`If-None-Match`一律用弱比较函数，故共用一个弱ETag仍能命中304），据此在D-14与九.9写明日后的压缩中间件接静态面无需换ETag。**两处补强**——`stdlib_web_sse_frames`的钉住头集补`Connection: keep-alive`（D-16的四个头之一，测试行漏钉）；3.1的闭合流守卫补全为三处（`stdlib/net.n:300`的`read`、`:367`的`consume`、`:381`的`peekWindow`都抛同一异常，同文案的原因是`:55`的`-1`映射只看错误码不看调用串）。**经核实撤销的7项**（留证防复审重燃）：`m_closing`复位缺口（`m_state`三态已**取代**它并接管复位，字段`:1527`、`close`置位`:1563`、accept循环检查`:1575`三处引用回源相符；第六轮回源更正：`:1563`是`close()`里的置位行，`start()`通体不查`m_closing`）；P8探针的rc=1被读成失败（该探针以`return c.hits;`收尾，rc＝1正是探针体被执行的设计信号，本文写的是「实测通过」而非「exit 0」）；ctn探针rc=1（普通腿累加到2、const腿累加0，退出码＝失败计数恰为设计断言，3.1:82的表述被支撑）；P5/skel3首轮rc=1（复审官的运行目录缺`wlib.ncu`所致伪象，按本文两TU配方在`skel3_build/`复跑四场景全绿；留下一条实施提醒——该e2e需要`order.txt`式两TU注册，先例`abstract_crossmodule_ok`）；§6.11的端口0＋`localPort()`（既有e2e语料如`stdlib_http_client.n:88-93`已全面钉死该用法，示例可运行性有既有测试背书）；cookie「RFC 6265 §5.4取首个同名」（§5.4的路径最长优先排序使「首个」良定义）；「库内`connectionClose`无复位点」（`HttpResponse`每请求新建，`serveOne`以`!res.connectionClose && reusable`收尾，`:1470`）。
- 2026-11-17 第六轮复审落实（1项中级＋7项低级＋5项参考级，十三项经回源全部成立，一次性改完；另有五项候选经复审官回源撤销（第五轮撤销的7项经抽验无一复活），清单与逐条出处见`temp/web_review_report_r6.md`）：**承载边界的import枚举补`math`**（F6-1，中级）——D-13与D-21都明令会话id与boundary取`math.randomBytes`，D-26那份枚举却漏了它；实施计划照枚举抄，`math.randomBytes`在编译期就是未解析符号。**三处第三方行为改写**（F6-2/F6-3/F6-4）——Go`FileServer`**不生成ETag**（`serveContent`只写`Last-Modified`，ETag归调用方自设后才参与条件校验；本轮改写时曾把条件校验的职能安给`writeNotModified`，第七轮回源更正：校验在`checkIfNoneMatch`/`checkPreconditions`里做，`writeNotModified`到304只做RFC 7232 §4.1的头清理），「弱ETag取大小＋mtime」原是按express/nginx一路错记到Go名下；nginx自动生成ETag但格式未取证（注:225/:397自认，且明令勿引mtime-size细节），故删去「nginx含inode」这种超出证据面的归属，只留注§4坑2那条有出处的警示（公式含inode或随机因子则每次重启缓存全废）；serve-static默认`maxAge=0`**仍**发`Cache-Control`（`cacheControl=true`），「不发定时max-age」与「不发`Cache-Control`」是两回事，本框架取的是Flask那一档姿态；优雅关闭行把Envoy的`--drain-time-s`/`--parent-shutdown-time-s`从nginx主语下摘回Envoy，nginx的对应强杀线补为`worker_shutdown_timeout`；SSE行的「nginx/PHP-FPM默认缓冲」缩回注§5逐字取证的那一半（nginx`proxy_buffering`默认开），fastcgi缓冲与gzip攒块两条标明「通识、注内无逐字出处」。**静态面405补`Allow`**（F6-8）——D-14、6.7与第七章矩阵三处此前只写405，与D-8自立的「405不带`Allow`不合规」及RFC 9110 §15.5.6的MUST（`temp/rfc9110.txt:7606`）冲突，现统一为带`Allow: GET, HEAD`。**引用精度**（F6-5/F6-6/F6-7/F6-10/F6-11）——九.11的416条款由不存在的§15.4.17改§15.5.17（`temp/rfc9110.txt:295`目录，注§4.2章首同款）；3.1的「排空」一词行手册节号分列为§23.6（`open`）与§23.4（`drain`），两词分处两章而原行只写一节；第五轮记录的「`start`检查`:1563`」改「`close`置位`:1563`」（`start()`通体不查`m_closing`）；3.1的url四函数行号与名单顺序逐一对位；`http.n:1444-1445`→`:1445-1446`、`:22-25`→`:23-26`（后者承重从句「writes the fields back」落在:26）。**转述精度**（F6-9/F6-12/F6-13）——Go的`ReadForm`限额维度改正（每部件10000属`NextPart`/`ReadPart`，`ReadForm`限的是全部文件部件头部数的合计10000）；「RFC 6265 §5.2.2规定要钳到0」改为该节对`Max-Age ≤ 0`的实际处置（即立即过期），第五轮记录同处一并更正；本轮写下的「过期时刻＝提取时刻」不是该节原话，自审取`temp/rfc6265.txt`回源更正为「最早可表示日期时间」（:1091-1092，见D-12）；D-21的HTML编码算法引文补全为整句（主语「The parts … that correspond to」与句尾「specified」都在原文，`temp/ref/src_mp/html_fci.txt:3059-3060`）。
- 2026-11-17 第七轮复审落实（1项参考级，十三项旧修正经逐项回源全部成立、无漏改亦无新损伤，另八项候选经回源撤销，逐项对照表见`temp/web_review_report_r7.md`）：**Go条件校验的函数归属**（F7-1）——第六轮改写静态服务那一行时，把「条件校验」安给了`writeNotModified`，而该函数在304时只做RFC 7232 §4.1的响应头清理（`temp/ref/src/go_fs_source.go:640-641`：响应已带ETag才删`Last-Modified`），真正比对`If-None-Match`的是`checkIfNoneMatch`（:557弱比较，只比响应里已设的ETag）经`checkPreconditions`（:648-678）调度；正文与本轮修订记录两处措辞据此改正并补上锚点。该行要立的事实不变：全文没有写ETag的语句（`Set("Etag")`零命中）、`serveContent`只调`setLastModified`（:278），故「Go默认不生成ETag、ETag归调用方自设」与本框架自定弱格式`W/"<size>-<mtimeMs>"`的裁决（D-14）都不受影响。
- 2026-11-17 引用补全自审（不开新复审轮：第七轮已判「可以进入实施计划」，本轮只把该轮撤销项6登记的「§5.2.2引文不可逐字核」补成可核）：**取得RFC 6265纯文本本地副本`temp/rfc6265.txt`**（curl自rfc-editor.org），据此更正D-12的`Max-Age`措辞——§5.2.2对`delta-seconds ≤ 0`是把过期时刻**直接置为「最早可表示日期时间」**（:1091-1092），再依§5.3「过期日期在过去即已过期」（:1318）与「MUST逐出过期cookie」（:1320-1321）无条件立即失效；此前两轮的「过期时刻＝提取时刻」既非该节原话，又恰好是本文要排除的「钳到0再按当前时刻＋0计算」那一档（它把过期时刻摆在提取时刻，是否失效交由收件人的比较符决定），故第五轮与第六轮记录的同处一并加注。`maxAgeSeconds`三态与`Session.destroy()`走负数写线上`Max-Age: 0`的结论不变，且因这段原文依据更足：对合规收件人无需赌实现差异。
- 2026-11-18 实施改名（实施计划级裁决1，落在优雅关闭那枚e2e语料）：`stdlib_web_drain_close`改名`stdlib_http_drain_close`，第七章该行行名同步。理由：这枚语料只用`http.HttpServer`裸面写，测试名的包前缀必须与行为拥有者一致——按包的影响映射（`tools/test_families.txt`）与手册`23-http.md`的引用都按名字里的包名定位，挂`web`前缀会让改`http.n`时选不到这条腿、也让手册指错拥有者；`web`门面自身的`drain`/`close`转发腿仍归后续的`stdlib_web_*`语料。上文各轮修订记录里的旧名不改，那是当时决定的原文。
- 2026-11-19 承载边界改判（用户裁定2026-11-19：stdlib包与文件一对一不成立，包可以嵌套导入、按java的方式组织，一个文件太大、上下文太长且不利维护）：作废D-26的「标准库不能是多文件包，故框架整体必须是单文件`web.n`」及其代价条，新增D-28定多模块分层——包级模块`stdlib/web.n`装它自己公开面点名的全部类型（`App`/`Router`/`RequestContext`/`Middleware`/会话族），三枚子模块`stdlib/web/static.n`、`stdlib/web/sse.n`、`stdlib/web/form.n`只单向import包级模块；分层的硬依据取自探针`temp/nest/`（一份`.nproj`同时装`k.n`与`k/w.n`可打包可运行、`import k.*;`并入整棵子树、包级模块反向import子模块被编译期拒为`import cycle detected: k -> k.w -> k`、只`import k;`时`k.w`不在名册里）；6.1公开词汇与6.11示例改模块限定名，第八章补`CMakeLists.txt`递归收集与`tools/test_families.txt`两处构建面。分层结论同时写回docs/roadmap.md的v0.13.12条与远期特性「标准库大文件改多模块包」条。
- 2026-11-19 名称解析修复（用户裁定2026-11-19：现有vm或编译器不合理的先改正，而不是迁就现有实现，不需要向下兼容）：新增D-29定三条规则——限定名按最长模块路径优先、其次类型分派并覆盖任意深度与全部成员种类，`import p.*;`对库包同样并入`p`及其子模块，诊断只对确实是模块路径的名字说「is not imported」。判定依据是实测矩阵（`temp/dlib/`、`temp/dmod_b/`、`temp/dmod_c/`）：`http.HttpResponse.reasonPhraseFor(404)`与`json.JsonValue.MAX_DEPTH`报`Module '…' is not imported`，而照该建议补import分别得到`not found`与`Wildcard import cannot target library package`，同形状的`p.K.s()`（项目模块）、`json.JsonKind.Object`（枚举成员）与裸类名`HttpResponse.reasonPhraseFor(404)`都能编译。据此作废实施计划裁决40「那是语言层面改动，本版不做」的结论与其在裁决44②的引用，手册`03-program-structure/index.md`§3.3「库包是单个单元」那句前提一并改写；D-27因此从「本版语言层面零变更」改为「无新语法，两处名称解析缺陷当版改正」，§九.18的三条递延语言层面缺口不变。同日补测两处：手册`04-types/enum.md`§4.6.4的`lib.Color.Blue.rank()`与`lib.Color.Red.toString()`在项目模块编译运行通过（探针`temp/d29p/p2/`退出码0），库包形态的`json.JsonKind.Object.toString()`却报`Module 'json.JsonKind.Object' is not imported`（探针`temp/d29p/`），据此规则1把「在成员常量上调方法」写入覆盖面；`docs/dev/name_resolution_design.md`§5的V3「库包禁通配」按其自陈理由在D-28之后失效，随规则2撤销、V2按规则1扩展，该文档另加带日期修订记录。
- 2026-11-19 实施期语言层面补登记（路由表编码时实测，探针`temp/t6_probe3.n`）：P2那句`function reference "…" requires an expected function type`还在**实例构造调用的实参位置**复现——`FuncMiddleware(this.routeTail)`这类`Type(args)`写法，自由函数引用与绑定方法引用一起被拒，而同一枚引用在普通调用实参、字段赋值与带类型的局部声明三处都能绑定，因此缺口是期望类型没有穿过构造调用那一格，与九.18第一条（带默认值的func形参）同属一条传播链。P2行的两处与九.18据实补写，第八章的roadmap清单由三条改为四条；框架侧不绕行编译器的办法只有一条一行式改写（先把方法引用落进`func<void, RequestContext>`局部变量，再交给构造调用），`App`构造`m_routeLink`即用此形制，故本版不受阻挡，缺口本身进roadmap远期特性。

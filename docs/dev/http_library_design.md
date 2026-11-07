# HTTP/1.1协议库设计(Spec)

**文档定位**:概要设计(Spec)。定义url包与http包及配套基础设施的目标、范围、架构裁定与验收标准;详细设计(逐任务实施计划)由本文件派生,另行制定。

**输入**:
- 用户指示:基于net库设计规划HTTP/1.1协议库,目标支持高并发服务和方便的客户端API;对性能不敏感的功能尽量用nlang实现,便于调试和用户查阅;按自主开发流程执行,自行裁定是否分成url、http和web框架库。
- 用户指示(整体架构):系统设计从nlang语言、工具链和标准库的整体架构考虑,不仅限于本库;若缺少通用字符串解析设施,应补充到标准库;字符串只是例子,其他基础设施缺口也要盘点。
- 用户裁定(流式):需要支持SSE,v1必须含流式消息体。
- 用户裁定(版本划分,2026-11-07):v0.13.3=Lambda函数/闭包、v0.13.4=字符串处理增强/URL库、v0.13.5=HTTP库、v0.13.6=WEB框架;本项目自0.13.4起,不等lambda。
- 用户裁定(基础设施,2026-11-07):gzip尽量采用主流开源库的实现;日期/时间设施需要建立专门的标准库。
- 用户裁定(基础设施改进,2026-11-07):改进标准库、语言、虚拟机和工具链的行为是受支持的,只要提出的需求是合理的;通用库需要时新增native亦可。
- 用户裁定(时钟命名,2026-11-07):`wallMs()`=墙钟(unix毫秒,等价CLOCK_REALTIME);`monoMs()`=单调钟,只用来算间隔、超时、定时器——既有`io.nowMs()`经此改名(不向下兼容);当时的`io.`前缀经2026-11-08时钟归属裁定改为`system.`前缀(见下条)。
- 用户裁定(项目目的,2026-11-08):开发http系列库的主要目的之一,是检验和改进nlang本身的基础设施;所有功能一律不向下兼容,合理性优先。
- 用户裁定(时钟归属,2026-11-08):时间相关函数不放`io`包,新建`system`包承载——经核实采纳(调研结论20):时钟非I/O,主流语言归系统设施(Java `System`)或专门time模块;nlang日历面已由`datetime`承载,原始计数器归`system`。环境变量、命令行参数、进程信息为`system`后续居民(本项目无消费者,按消费者立项)。
- 用户裁定(内核最小化,2026-11-08):除语言本身需要的基础设施和指令要内建于nvm外,需在C++实现的功能一律放独立库;VM内建注册仅限有明确理由(vm本身依赖,或极致性能优化);spec不合理处可调整。应用:`system`时钟native改由独立`nlang_system.dll`提供,与既有native模块形态统一(取代v2.9「VM内建注册、零新DLL」方案,D20/D23);string三方法经用户裁定保持intrinsic——「string本身就是语言的一部分,需要做成intrinsic」,属「vm本身依赖」的明确例外(D4/D23)。
- 验收参照:`docs/ref/http_verify.md`(url/http/web三库验收规范,B/M/m分级,高并发/SSE/混沌专项)。
- 既有成果:net库(v0.12.9,stdlib/net.n)、io包字节设施、M:N虚拟线程调度器、`func<R, P…>`函数值与绑定方法引用。

**裁定原则**:不向下兼容;合理性优先,不随便把功能降级为「已知限制」;现状不是设计依据;KISS;性能不敏感的功能归脚本层(标准库形式),性能敏感的归native(如压缩、传输)。

**状态**:v2.10,并入用户内核最小化裁定与string方法形态裁定(2026-11-08)后的修订轮,待循环完善审核后定稿(前九轮记录见修订记录)。

---

## 一、目标与范围

### v1做

1. `url`包:RFC 3986通用语法URL解析、构件重组、相对引用解析、百分号编码/解码、表单编码查询串解析与构造。
2. `http`包:HTTP/1.1协议库,含消息模型(请求/响应/头部,**缓冲与流式双形态**)、客户端(连接池、重定向、分级超时、流式收发、便捷函数)、服务端(高并发、keep-alive、加固解析、chunked流式响应、背压与取消传播、100-continue)。
3. 配套基础设施(盘点见第六节):`string`新增`indexOf(t, from)`、`toInt(radix)`、`equalsIgnoreCase(t)`三方法(D4);`io`包新增base64编解码,`io.nowMs`移出改名`system.monoMs`;新增`system`时钟包(`wallMs`墙钟/`monoMs`单调钟,D20);新增`datetime`专门包(D20);新增`gzip`包并集成http内容编码(D21)。
4. 示例(http_hello、http_conc_bench、http_sse、url_tour)、e2e测试、用户手册新章节。

### 版本划分(用户裁定2026-11-07,D22)

| 版本 | 内容 |
| --- | --- |
| v0.13.3 | Lambda函数/闭包(独立语言子项目,本项目不依赖) |
| v0.13.4 | 字符串三方法+io.base64+`system`时钟包+`datetime`包+`url`包(含手册15-system、22-url、23-datetime章,§17.2与§20增补) |
| v0.13.5 | `http`包全部(分帧、服务器、客户端+池+重定向、流式+背压+取消、加固)+`gzip`包及http集成(含手册24-http、25-gzip章与§20) |
| v0.13.6 | WEB框架(独立子项目,lambda就位后启动) |

各版本公开面是最终面的子集,后版只增不改——虽不承诺向下兼容,也不自破刚发布的API。

### v1不做(各有独立理由,非「已知限制」式递延)

| 不做 | 理由 |
| --- | --- |
| TLS/HTTPS | 加密后端选型(SChannel等)是独立子项目;v1传输面直连TcpStream,接缝设计随TLS子项目一并裁定(D17)。 |
| HTTP/2、WebSocket | 协议不同层;HTTP/2不复用HTTP/1.1分帧,WebSocket需要升级握手与帧层,各自独立立项。 |
| web框架(路由、静态文件、中间件) | 独立后续子项目,排0.13.6(D1/D22);中间件链依赖lambda闭包,协议包保持职责干净。 |
| SSE helper(event-stream编码封装) | HTTP层只提供chunked流式能力,不理解SSE语义(http_verify §6/SSE-008);helper归web框架,本项目以`http_sse.n`示例演示手工framing。 |
| MIME content-type嗅探 | 消费者是web框架静态文件服务(0.13.6),届时随框架立项。 |
| datetime本地时区 | 需要操作系统时区native;v1为UTC基准(D20),本地时区为roadmap后续扩展。 |
| gzip流式压缩 | v1压缩只作用于缓冲body(D21);流式gzip需要flush边界控制,随流式设施演进而非v1核心。 |
| 客户端pipelining | 业界已死(D11调研:cURL 7.65移除、Chrome/Firefox放弃,HTTP/2多路复用取代)。 |
| CONNECT/代理 | v1无代理场景;协议面留待将来。 |
| IDN、WHATWG URL特殊规则 | 与RFC 3986互不兼容(D3调研),选RFC 3986即排除。 |

---

## 二、关键事实(勘察结论)

1. **传输面**:`TcpStream`已具备HTTP所需全部原语——`read(buf, timeoutMs)`、`readExactly(count, timeoutMs)`、`readLine(timeoutMs)`、`write(byte[]/string, timeoutMs)`(write-all语义;byte[]重载在`timeoutMs`前有可选`count`参数指定写入字节数,net.n:297)、`close`、`shutdownWrite`、`isClosed`、`setNoDelay`、`setKeepAlive`(`stdlib/net.n:252-420`)。`readExactly`直接服务定长body,`isClosed`服务连接池借出探测。
2. **字节语义**:`s[i]`返回该字节偏移的`ubyte`(`04-types/string.md:95`),字符串按字节索引;`io.toBytes/fromBytes`、`io.nowMs`(0.13.4移出改名`system.monoMs`,时钟归属裁定见D20)、`io.ByteStream`(内存字节缓冲)可用。
3. **函数值**:函数类型写作`func<R, P…>`(§7.4);绑定方法引用捕获接收者,状态经字段持久——`examples/net_http_mini.n`已用`HttpConnection.run`绑定方法做handler。lambda闭包是下一版特性(v0.13.3),故v1 handler状态一律经绑定方法+类字段。
4. **异常树**:`lang.n`无`IllegalArgumentException`;按包定义异常的既有先例是`net.n`的`SocketException`/`TimeoutException`。
5. **纯nlang包无需DLL**:VM仅在限定名native查找未命中时才触发`nlang_<ns>.dll`加载(`src/vm/VmExecutorNativeHost.cpp:423-435`);纯脚本函数编译为字节码,不触发加载。`tensor.n`已含纯nlang函数层(relu/softmax),但**整包纯脚本无先例**,实施计划需先探针验证npkg打包与加载路径。
6. **字符串方法面**(§16.2):已有`substring/indexOf(t)/startsWith/endsWith/contains/toUpper/toLower(ASCII)/trim/split/replace/toInt()/toLong()/toFloat()/toDouble()/charAt/charCount`;**缺**`indexOf(t, from)`(缓冲扫描必需)、**缺**按进制解析(`"1a".toInt()`失败,百分号解码与chunked分帧都要十六进制)、**缺**大小写不敏感比较(头部名/方法/scheme按RFC 9110大小写不敏感)。Phase 11曾裁定「string方法面冻结」,本设计按D4解冻。
7. **并发设施**:`Channel<T>`容量参数化——capacity≥1为有界,`send`在缓冲满时阻塞(=天然背压);capacity 0为无界(`concurrent.n:59-60`);通道关闭后对已关通道`send`/`recv`抛`ChannelClosedException`;`recv`无超时参数——生产者必须以`finally`关通道兜底,否则消费者永久阻塞(D18)。
8. **先例**:`examples/net_http_mini.n`是手写HTTP/1.0迷你服务器+客户端(readLine解析请求行、readExactly取body、port 0+localPort、accept超时轮询+SocketException退出),即本包要一般化的雏形。
9. **索引钉**:`test_library_index.cpp`钉库索引267个自由函数,类方法不计——string方法新增不动此钉,url/http/gzip/io.base64/system.wallMs的自由函数会动(`io.nowMs`移出改名`system.monoMs`为净零),计划精确重算。
10. **手册结构**:第二部分(标准库)止于第20章网络库;第三部分二进制格式为21/22/23章。0.13.4新增`system`章(插于14-io之后为15,现15-20章顺延为16-21)与url/datetime两章(22/23),格式章顺延为24/25/26;0.13.5新增http/gzip两章(24/25)后再顺延为27/28/29。重编号波及的引用面散布中英两树数十个文件(计数随形态口径而变,spec不钉死),含`§N.M`、`第N章`、`chapter N`(大小写混用)三种节号形态、文件名链接、被重编号章自身的内部节号自引用;陈旧节号的链接本身有效,linkcheck与nav-coverage均测不出,会静默出厂——故同步按§十一.3的规则由实施计划grep列全,spec不手写计数(手写清单已两轮漏项)。
11. **测试登记**:`tests/e2e/manifest.txt`与`tests/e2e/examples_manifest.txt`格式为`name expected_exit_code [stdout_substring]`;net族e2e用例命名`stdlib_net_*.n`配`.flags`。
12. **版本排期**:用户裁定(2026-11-07)总体划分见第一节版本划分表;本项目落点0.13.4/0.13.5,roadmap按此登记。

---

## 三、调研结论与出处

| 结论 | 出处 |
| --- | --- |
| 分层先例普遍为「URL独立包+协议库+薄框架」:Rust分url/http/hyper/axum(axum是hyper上的薄封装);Go分net/url与net/http(路由仅是薄ServeMux);Node分url与http模块;PHP 8.5独立URI扩展。 | [axum与hyper关系](http://blog.csdn.net/gitblog_00023/article/details/138944677)、[hyper定位](https://zhuanlan.zhihu.com/p/711528388)、[Rust Web分层](https://blog.gitcode.com/ff9125df326419b40c45d4db51fd14a0.html) |
| RFC 3986与WHATWG URL标准互不兼容:Go net/url(遵RFC 3986)对top-100k URL与WHATWG有1398处分歧;PHP 8.5同时提供两套。选RFC 3986即排除WHATWG特殊规则。 | [Go与WHATWG分歧](http://mp.weixin.qq.com/s?__biz=MzAxMTA4Njc0OQ==&mid=2651456120&idx=2&sn=f4f8d62cdfc3729559443937d2104f8e)、[标准之争](https://learnku.com/articles/91665)、[PHP双实现](https://www.cnblogs.com/catchadmin/p/19149230) |
| 纯脚本HTTP服务器有成熟先例:Python http.server纯stdlib实现(仅因安全检查有限而不推荐生产),Ruby WEBrick曾是stdlib服务器。nlang具备绿线程+阻塞IO挂起+字节语义,基座强于上述两者。 | [http.server说明](https://www.bookstack.cn/read/python3.9-library-zh/690244dde0974416.md)、[生产限制讨论](https://blog.csdn.net/weixin_33462167/article/details/167660205) |
| 请求走私(CL.TE/TE.CL/TE.TE)根因是Content-Length与Transfer-Encoding并存时的解释分歧;防御是拒绝二者共存、TE只接受恰一个chunked。 | [请求走私机理](https://blog.csdn.net/liwenxiang629/article/details/150949753)、[CL.TE/TE.CL](https://www.cnblogs.com/mrhonest/p/13566091.html)、[走私防御](https://cloud.tencent.com/developer/article/2288250) |
| 头部上限业界一律回431:Go `MaxHeaderBytes`默认1MB(含请求行)超限→431;Go 1.27新增`MaxHeaderValueCount`默认每键100值,超限→431(header-flood加固,Envoy CVE-2024-27919同类攻击面);Node `--max-http-header-size`默认8KB超限→431;nginx 1KB/4×8k。 | [Go MaxHeaderBytes](https://www.17golang.com/article/638611.html)、[Go 1.27值数上限](http://mp.weixin.qq.com/s?__biz=MzIxMzgyNTQyOA==&mid=2247497852&idx=1&sn=99cdfca03aa8a2f07b23e5f2ccb891d8)、[Node 431](https://cloud.tencent.com/developer/ask/sof/108040490) |
| 超时分级是主流共识:OkHttp分connect/read/write各默认10s+call(整请求含重定向,默认0=关);Go分Dial/ReadHeader/Read/Idle;Node分keepAliveTimeout(空闲)/headersTimeout(头部接收)/requestTimeout(整请求)三时限。空闲时限与头部时限必须分离(Slowloris防御)。 | [OkHttp超时](https://blog.csdn.net/weixin_31188927/article/details/168221648)、[Node超时调优](https://xiaci.blog.csdn.net/article/details/154244346) |
| 流式/SSE服务器模型:Go ResponseWriter默认缓冲,逐事件显式`Flush()`;写失败(EPIPE)=客户端离开=handler退出信号;心跳用`:`注释行;取消经`context.Done()`。有界通道泵=背压感知的Flusher。 | [Go流式实战](https://blog.csdn.net/weixin_34098209/article/details/168237009)、[Go chunked与Flush](https://www.php.cn/faq/2222503.html) |
| body关闭/排干纪律:Go要求Response.Body必Close且读到EOF才可复用;Go 1.27为Close增加有限自动drain(≤256KB或≤50ms,`Connection: close`时跳过);服务器侧`maxPostHandlerReadBytes`=256KB有限排干再复用连接,超限关连接。 | [Go 1.27 Close行为](http://mp.weixin.qq.com/s?__biz=MzIxMzgyNTQyOA==&mid=2247499441&idx=1&sn=3dd773660ffa6c748e0d58d3a75372fd) |
| 百分号编码按组件分集:Python `quote(s, safe='/')`(表单用`quote_plus`空格→`+`)、Go `PathEscape`/`QueryEscape`按上下文shouldEscape。保守的unreserved-only编码在任何组件都合法(可能过度编码),作为通用面是刻意简化。 | [Python quote](https://www.bookstack.cn/read/python3.9-library-zh/690244dde0974416.md)、[Go base64/URL编码实践](https://www.17golang.com/article/509318.html) |
| 字符串方法语义先例:Java `indexOf(str, fromIndex)`(from<0按0,from>length返回-1)、`Integer.parseInt(s, radix)`(2..36)、`equalsIgnoreCase`;Python `find(sub, start)`、`int(s, base)`同形。 | [Java String/Integer](https://docs.oracle.com/javase/8/docs/api/java/lang/String.html)、[Python int](https://docs.python.org/3/library/functions.html#int) |
| base64设施位置:Java `java.util.Base64`、Go `encoding/base64`、Python `base64`模块——均为通用工具/编码位置,提供标准字母表+URL安全字母表±填充变体(RFC 4648)。 | [Go encoding/base64](https://www.kancloud.cn/lixp/golang/801638)、[Go URL安全变体](https://www.17golang.com/article/634765.html) |
| chunked上传:Go请求Body非nil且未设Content-Length→自动chunked;部分服务器/框架不支持chunked请求体(转发场景需显式Content-Length)。已知长度优先Content-Length。 | [Go chunked上传](https://global.php.cn/zh-tw/faq/1797058704.html)、[CL与chunked演示](https://blog.csdn.net/qq_41126242/article/details/150583207) |
| 连接数上限:nginx `limit_conn`超限默认回503(`limitconnstatus`默认503);Go `LimitListener`是accept阻塞式(无HTTP语义)。HTTP层拒绝取nginx先例=503。 | [nginx limit_conn](https://juejin.cn/post/7220262023471661117)、[Go LimitListener](http://www.caotama.com/2733290.html) |
| pipelining已死:cURL 7.65移除支持,Chrome/Firefox放弃,HTTP/2多路复用取代。 | [pipelining弃用](https://blog.csdn.net/cpongo4/article/details/89496318) |
| 100-continue:libcurl对>1KB body发Expect,部分服务器回417,客户端不可无限等待。 | [libcurl Expect](https://blog.csdn.net/lpruia/article/details/18006633)、[417陷阱](https://blog.51cto.com/u_15274085/2919200) |
| 客户端池化:Go MaxIdleConnsPerHost默认2、IdleConnTimeout 30s,且客户端空闲超时必须短于服务器keep-alive超时,否则竞态;池按host:port分键;达`MaxConnsPerHost`上限时请求入wantConn队列阻塞等待归还,无请求deadline即无限等待。 | [Go Transport默认值](https://www.php.cn/faq/2620817.html)、[池调优](https://blog.csdn.net/2301_77759937/article/details/160086338)、[空闲超时关系](https://www.php.cn/faq/2467184.html)、[wantConn队列机制](http://www.shadafang.com/a/bb/0R222D5432020_2.html)、[达上限阻塞等待](https://blog.csdn.net/qq_31142761/article/details/166016216) |
| 重定向:Go默认跟随10次,301/302/303改GET、307/308保持方法,跨主机剥离Authorization/Cookie;CheckRedirect回调。 | [Go重定向策略](https://juejin.cn/post/7408391774635065396)、[SSRF注意](https://www.php.cn/faq/2693445.html) |
| 透明gzip的注入与解压绑定:Go transport在发送层自动添加`Accept-Encoding: gzip`(不在用户可见Header表),且仅当自己注入时才透明解压——用户显式设置`Accept-Encoding`即放弃自动解压;`DisableCompression`只关注入。 | [Go DisableCompression语义](https://blog.csdn.net/iUcool/article/details/139003037)、[DisableCompression用法](https://cloud.tencent.com/developer/ask/sof/112604085) |
| 验收框架:`docs/ref/http_verify.md`给出三库用例编号体系(URL-*/HTTP-SRV-*/CONN-*/CLI-*/CHK-*/HC-*/SSE-*/HTTP-S*/P*)、B/M/m分级与三条红线(增量解析半包粘包、背压、取消传播);其§11分期(TLS二期/HTTP2三期/WebSocket二期/compress二期)与本设计排除面吻合。 | `docs/ref/http_verify.md` |
| 时钟函数归属:Java `System.currentTimeMillis()`(墙钟)/`System.nanoTime()`(单调)、.NET `Environment.TickCount64`/`Stopwatch`、Node `Date.now()`/`process.hrtime()`、Go `time.Now()`(含单调读数)、Rust `std::time`(SystemTime/Instant)、Python `time.time()`/`time.monotonic()`、C++ `std::chrono`——时钟一律归系统设施类或专门time模块,无一置于I/O模块;墙钟与单调两域分工(时间点vs间隔)亦是共识。nlang日历面已由`datetime`承载,原始计数器取Java `System`形态归`system`包。 | [Java两钟分工](https://www.php.cn/faq/2636037.html)、[Java currentTimeMillis/nanoTime](https://www.php.cn/faq/2793698.html)、[.NET Environment.TickCount64](https://learn.microsoft.com/dotnet/api/system.environment.tickcount64)、[Rust std::time](https://doc.rust-lang.org/std/time/index.html)、[Go time](https://pkg.go.dev/time)、[Python time](https://docs.python.org/3/library/time.html) |

---

## 四、架构裁定

| # | 裁定 | 理由 |
| --- | --- | --- |
| D1 | 分三层:`url`包与`http`包入本项目,web框架(路由/静态文件/中间件/SSE helper)为独立后续子项目,排v0.13.6。 | 调研结论1:url独立、协议独立、框架薄层是各语言一致的分层;框架中间件链需要闭包捕获(v0.13.3),现在只能退化为绑定方法链,设计价值有限;协议包不含路由保持职责干净(Go ServeMux亦仅为薄封装,后加不破协议层)。 |
| D2 | url.n、datetime.n与http.n为纯nlang实现:零新增native、零新增DLL;解析、分帧、客户端、服务端全在脚本层。gzip为例外(D21,native)。 | 用户指示(性能不敏感功能用nlang,便于调试与查阅);调研结论3(Python/WEBrick先例);事实5(loader按native未命中触发DLL,纯包天然无需)。逃生口:若conc基准显示解析为热点,热路径可日后native化而API不变。用户裁定(2026-11-07):改进标准库/语言/VM/工具链行为受支持,只要需求合理——本裁定「零新增native」是合理需求下的优先选择而非禁令(system墙钟native即按此裁定新增)。跨包复用native(`datetime.now()`复用`system.wallMs`(新增墙钟native,D20)、http的`bodyText`复用`io.fromBytes`(既有))经`import system;`/`import io;`——预置包间互导入现无先例(stdlib全包零互导入),计划探针任务一并验证;若加载路径不支持,回退为消费包内声明该native并由其命名空间对应的`nlang_<ns>.dll`提供(库索引钉数与DLL清单须计入,D23)。 |
| D3 | URL遵RFC 3986通用语法(Go net/url路线):组件拆分存储(scheme/userInfo/host/port/path/query/fragment)、一般语法相对引用解析、unreserved集百分号编码、查询串按application/x-www-form-urlencoded对解析。不做IDN punycode转换——非ASCII主机按字节原样保留交上层(URL-010两策略取「原样保留」档并文档化,DNS失败在连接阶段显现);不做WHATWG特殊规则。 | 调研结论2:两标准互斥,WHATWG需要IDN/punycode与Unicode规范化整套设施,超出v1体量;RFC 3986确定性强、实现小,与nlang字节语义吻合。 |
| D4 | 补三个通用字符串方法到`string`:`indexOf(t, from)`、`toInt(radix)`、`equalsIgnoreCase(t)`。 | 用户指示(缺通用解析设施应补标准库)。每个在本项目有≥2消费者:indexOf带起点——url组件边界+http头部行扫描(无它则缓冲扫描退化为反复substring,易错且O(n²));toInt(radix)——百分号解码+chunked十六进制分帧;equalsIgnoreCase——头部名/方法/scheme大小写不敏感(RFC 9110),现用toLower()比较每次分配。语义钉死为Java先例(调研结论10):from<0按0、from>length返回-1;toInt严格整串2..36进制、畸形/溢出抛`Exception`(与既有`toInt()`家族同行为,手册§17.2——system章插入重编号后的落点)。已评估通用tokenizer/Scanner:不需要,indexOf+substring+循环足够覆盖,再加抽象属臆测泛化。解冻Phase 11「string方法面冻结」裁定:该裁定防的是无序膨胀,本三项各有明确消费与调研依据,合理性优先。实现形态(用户裁定2026-11-08):三方法保持String面类方法(归`lang`库)并做成intrinsic——string是语言基本类型,其成员操作属内建于nvm的语言基础设施,属内核最小化裁定(D23)「vm本身依赖」的明确例外(string接收者是字符串池索引而非对象,其成员调用只经内建函数表分派,见§七);不做包自由函数。 |
| D5 | 消息模型双形态(缓冲+流式)。请求体:服务器侧`HttpRequest.bodyReader`为分帧感知reader(`read`/`readAll`/`bodyText`/`close`),handler可只读所需部分;`maxBodyBytes`仅在readAll处强制(readAll超限→413);连接循环排干的上限是D19的固定256KB(与`maxBodyBytes`配置无关,排干超限→关连接——响应已由handler定夺,不再改状态码,D19)。响应体:`byte[]`(写出Content-Length)或有界`Channel<byte[]>`(chunked流式,send阻塞=背压)。客户端:缓冲档`request()`内部readAll;流式档`open()`交回`bodyReader`;流式上传经`bodyChannel`(chunked)。 | 用户裁定(需支持SSE,v1含流式);http_verify §6/SSE-008:HTTP层只提供chunked流式能力,不理解SSE语义;调研结论7(Go逐事件Flush模型——有界通道泵即其背压等价物)、结论8(close/排干纪律)。缓冲档覆盖主流工作负载,流式档覆盖SSE/大文件,两档同源于同一reader/分帧实现,无重复逻辑。 |
| D6 | 并发模型:accept线程+每连接一虚拟线程;handler抛异常→回500并关闭该连接(body超限异常按D5回413,不属此列)。 | 与net_conc_bench/net_http_mini同形,Loom/goroutine先例(虚拟线程使阻塞式每连接一线程廉价);handler异常后连接状态不可知,关闭是Go panic处理的先例。 |
| D7 | keep-alive:HTTP/1.1默认持久连接;空闲超时`keepAliveTimeoutMs`默认30s;头部阶段另有总时限`headerTimeoutMs`默认10s——请求行+头部未在时限内收齐即断连(Slowloris);body阶段另有每次读间隔时限`bodyTimeoutMs`默认10s(0=不限)——body细水慢灌与半开连接同样有界;响应写出(含流式泵)用分段时限`writeTimeoutMs`默认10s(0=不限),body按≤64KB分段写出,时限是每段间隔而非总时长,超时视为客户端离开(走D18取消路径);新连接一律`setNoDelay(true)`+`setKeepAlive(true)`(net.n二者仅收bool,探测间隔为OS默认,keepalive只作半开兜底,快速发现靠读写时限);尊重`Connection: close`;HTTP/1.0默认关闭、显式keep-alive才持久(RFC 9112 §9)。 | 持久连接是HTTP/1.1默认行为;空闲与头部两时限分离是Go `ReadHeaderTimeout`与Node `headersTimeout`的共同设计(调研结论6),合并为一参会让慢速头部客户端占住空闲额度;body与写出时限补齐Go `Server.ReadTimeout`/`WriteTimeout`的旋钮集——只有头部时限则body阶段Slowloris(HTTP-S02)与半开推送(HTTP-HC-004)无界;每次读间隔/每段写出语义比Go的总时长语义更友好(慢而稳的对端不误杀,细水慢灌仍被切断);各时限均以带deadline的虚拟线程读写实现——VM调度器统一管理deadline,空闲连接是parked虚拟线程而非per-connection定时器,HTTP-HC-003「CPU不随连接数线性上涨」语义满足。 |
| D8 | 客户端连接池:`HttpClient`内按host:port分键,每主机空闲上限4、空闲超时10s(v1固定常量,不设旋钮——池调优面随消费者出现);借出时`isClosed()`探测,已关则丢弃新开;不做自动重试。 | 调研结论16:客户端空闲必须短于服务器空闲——服务器默认30s(D7),客户端10s留足余量;活跃连接默认不设上限(Go `MaxConnsPerHost`默认不限先例),保留`maxConnsPerHost`旋钮(0=不限):设值时池满借出阻塞等待归还,超`callTimeoutMs`抛`TimeoutException`,`callTimeoutMs=0`则无限等待(Go达上限入wantConn队列阻塞、无deadline即恒等待同构,调研结论16)——HTTP-HC-016池满排队行为可表达可测(见第九节适配清单);Go的30s依赖其后台探测机制,我们没有,故取短值并以isClosed探测兜底;不自动重试保持失败语义明确(用户可见异常即真实失败)。 |
| D9 | 解析加固(服务器):严格CRLF;头部行内裸CR/LF拒绝(400);名称/值含控制字符或`\0`拒绝(400);obs-fold续行拒绝(400,RFC 9112 §5.2);OWS容错(`Name:  value`冒号后与值前后空白剥离,HTTP-SRV-015);冒号前空白(`Name :value`)不合RFC 9112 §5.1 field-line语法,按畸形头部拒绝(400,HTTP-SRV-015拆分判定见适配清单);Content-Length与Transfer-Encoding并存拒绝(400);TE只接受恰一个`chunked`(值大小写不敏感),否则400;请求行+头部总量≤`maxHeaderBytes`默认64KB→**431**;头部条数≤`maxHeaders`默认100→**431**;HTTP/1.1请求必须恰有一个合法Host(400);方法必须是合法token(400;大小写敏感、不限全大写,HTTP-SRV-002适配见第九节适配清单);未知协议版本→505;request-target接受origin/absolute/`*`三形式;`Connection`按逗号多token解析(`close`在任意位置生效,未知token透传);`Upgrade`头部保留对handler可见,库不执行升级握手(HTTP-CONN-009适配:等效拒绝而非静默)。写侧(客户端与服务器):头部名/值含CR/LF即抛(防消息分裂,HTTP-S03)。HEAD/304/204的body抑制规则见§八。 | 调研结论4(CL∧TE是走私根因)、结论5(431是Go/Node共同选择;条数上限对应Go 1.27 `MaxHeaderValueCount`加固)、结论13(nginx 503先例);64KB介于Node 8KB与Go 1MB之间,容纳cookie/JWT场景又远低于内存风险。 |
| D10 | 100-continue:服务器对`Expect: 100-continue`先发`100 Continue`再读body,未知Expect回417;客户端不发Expect,但容忍1xx中间响应(跳过取最终响应)。 | 调研结论15:服务器支持成本低;客户端不发避免额外往返与417陷阱,容忍1xx保证对真实服务器的健壮性。 |
| D11 | pipelining:客户端不pipelining;服务器不特设机制——缓冲读取器保留已读入的后续字节,连接内顺序处理即自然兼容。 | 调研结论14:pipelining已死,不值得客户端投入;服务器兼容是缓冲读取器的免费副产品。 |
| D12 | 重定向:客户端默认跟随,上限10(`maxRedirects`可配,0=禁用);301/302/303改GET并丢弃body(可重放);307/308保持方法——缓冲`body`可重放,`bodyChannel`已消费不可重放→抛`HttpException`(Go「not replayable」同行为);跨主机剥离Authorization与Cookie;相对Location经`Url.resolve`解析。 | 调研结论17:Go规则是业界事实标准;跨主机剥离凭据是SSRF基本防御。 |
| D13 | 异常:`url.n`定义`UrlException : IOException`(解析/解码失败),`http.n`定义`HttpException : IOException`(协议错误:畸形响应行/头部、重定向超限、头部注入字符、body超限),`datetime.n`定义`DateTimeException : Exception`,`gzip.n`定义`GzipException : IOException`;`SocketException`/`TimeoutException`透传;4xx/5xx不是异常。 | 事实4:按包定义异常是既有先例;状态码是响应不是错误(requests/Go一致行为)。 |
| D14 | 服务器生命周期:`HttpServer`实例先配字段再启动——`start(host, port, handler)`非阻塞(accept线程)供测试与同进程场景,`serve(host, port, handler)`阻塞供独立服务器进程(main无需sleep占位);两者均可被另一线程的`close()`唤醒停机(accept经SocketException唤醒,net_http_mini模式);支持port 0+`localPort()`;handler类型`func<HttpResponse, HttpRequest>`;`maxConnections`超限→503+关连接(nginx先例,调研结论13),`maxRequestsPerConnection`超限→末响应`Connection: close`(nginx keepalive_requests先例)。 | main返回即进程退出(§9.3),独立服务器必须有阻塞入口(Go ListenAndServe、Python serve_forever先例);port 0+localPort是e2e/文档围栏自终止的前提(事实8);实例配字段避免静态start的参数鸡生蛋;类类型作函数类型实参有thread.n `func<T>`先例,计划探针确认。 |
| D15 | 客户端API分两档:便捷自由函数`http.get/post/put/del/request`(共享默认客户端,缓冲档;`timeoutMs`参数=整请求上限)与`HttpClient`类(池、默认头部预置User-Agent——请求自带头部优先、默认只补缺,`Accept-Encoding`由缓冲档发送时注入(D21)、maxRedirects、`maxConnsPerHost`、流式档`open()`)。超时三旋钮:`connectTimeoutMs`默认10s(连接阶段)、`readTimeoutMs`默认10s(每次读间隔,0=不限,SSE长连接调大)、`callTimeoutMs`默认0(整请求含重定向,0=不限)。`HttpRequest`/`HttpResponse`为类。 | 用户目标「方便的客户端API」:requests式一行调用+可配置对象两档是requests/Go/OkHttp共同形态;超时分离是OkHttp/Go/Node共识(调研结论6),整请求超时与SSE长连接互斥故必须拆;`del`避开`delete`关键字风险(计划核实)。 |
| D16 | chunked双侧读写:读侧——客户端解析chunked响应(含trailer丢弃),服务器解析chunked请求体;写侧——服务器流式响应(`bodyChannel`)与客户端流式上传(`bodyChannel`)均chunked写出,写出侧无缓冲body时恒chunked,有缓冲body恒Content-Length(已知长度优先,调研结论12)。trailer读侧丢弃、写侧不发(v1);chunk extension(`5;foo=bar`尺寸行参数)兼容解析并忽略,不当数据(Go先例,HTTP-CHK-004)。 | 流式形态下写出侧长度未知,chunked必需(用户裁定流式);读侧必须支持因为对端(curl等真实客户端/服务器)会发chunked。 |
| D17 | TLS接缝:v1传输面直连TcpStream,不预先引入流接口;v1客户端对非`http` scheme(含`https`)在请求组装时抛`HttpException`拒绝;TLS子项目落地时连同其需求(ALPN、握手形态)一并设计接口,机械替换并开放https。 | 避免投机泛化:接口方法集应由TLS真实需求决定;现在定义一个用不上的接口是YAGNI违背;显式拒绝优于明文连上https端口挂到超时(错误信息明确指向缺TLS)。 |
| D18 | 背压与取消传播:流式响应经有界`Channel<byte[]>`(容量≥1——capacity 0=无界会静默失去背压,不采纳;通道由handler构造并赋值给`bodyChannel`,推荐容量8块),pump(连接线程)逐块`recv`→chunked写出→flush语义(每块即发);`send`阻塞=背压,慢客户端最终阻塞生产者(HTTP-HC-011/SSE-004)。取消:pump写失败(客户端断开)→close通道→生产者`send`抛`ChannelClosedException`→handler/生产者线程停止(SSE-002/HTTP-HC-013)。生产者必须以`finally`关通道(`recv`无超时,事实7;泄漏=连接永久挂起);生产者阻塞在他处到不了`finally`时pump的`recv`停摆——界定以生产者契约为主:handler拥有两线程、必须保证生产者终结,违约属调用方缺陷,库的兜底是每违约请求至多泄漏一个连接;`maxConnections`仅为部署侧可选上限(默认0=不限时不构成封顶)。 | 用户裁定流式/SSE;调研结论7:Go以写失败+context为退出信号,通道泵是其背压等价物;取消必须双向可观测是http_verify红线2。 |
| D19 | 连接复用纪律:客户端`bodyReader.close()`——body已耗尽→回池;未耗尽即close→直接关连接(取消载体,HTTP-CLI-011:客户端取消传播到连接层,socket及时关闭不泄漏;缓冲档`readAll`恒耗尽,故复用不受影响)。服务器侧handler未读完请求体→连接循环有限排干(≤256KB,Go `maxPostHandlerReadBytes`先例,调研结论8)再复用,排干超限或出错则关连接。出错连接一律不回池。`HttpClient.close()`连同在途连接一并关闭,阻塞中的调用经`SocketException`失败(OkHttp `Call.cancel()`同形)。 | 复用与正确性互锁:未耗尽body的连接回池会把残留字节混进下个请求;服务器侧无界排干让大请求体拖死连接,故取Go的256KB有限值;客户端未耗尽即close语义唯一化为取消(排干复用与取消观测不可兼得,取取消——HTTP-CLI-011为B级,复用损失仅见于流式档主动放弃响应)。 |
| D20 | 日期/时间建专门`datetime`包(用户裁定2026-11-07):`DateTime`不可变值(UTC日历字段+epoch毫秒),`now()`(复用`system.wallMs`墙钟native——既有`io.nowMs`是steady_clock单调钟,任意起点、非墙钟(`io.n:163-165`、`VmExecutorNativeCallbacks.cpp:380-385`),无法产出日历字段,按用户裁定移出io并改名`system.monoMs`(时钟非I/O,归系统设施,调研结论20;只用于间隔/超时/定时器);新建`system`包承载二者,`wallMs`为新native,等价CLOCK_REALTIME(用户裁定2026-11-07/11-08:改进标准库/VM行为受支持,只要需求合理;不合理名称与归属不因现有实现维持,不向下兼容);`wallMs`/`monoMs`均为普通脚本native,按net库先例由新建`nlang_system.dll`提供(用户裁定2026-11-08:wallMs不属于语言的一部分、无内建理由,时钟实现形态与既有native模块统一,D23),跨包复用路径见D2)、epoch换算、`addMillis`日历运算(days_from_civil/civil_from_days算法,主流C++/fmtlib采用)、ISO 8601 `toString`/`parse`;v1为UTC基准,本地时区需OS时区native,roadmap后续。HTTP的RFC 1123日期格式(`Date`/`Expires`)归http.n实现(协议专属),建于datetime日历原语之上。 | 用户裁定「日期/时间设施需要建立专门的标准库」——内部helper不可发现、不可复用;日历运算是通用设施,与http无关也应存在;UTC-only是范围裁定而非限制(本地时区列roadmap)。 |
| D21 | gzip建`gzip`包,native实现采用捆绑zlib(用户裁定2026-11-07「尽量采用主流开源库的实现」):压缩是CPU密集循环+位IO,属性能敏感面,符合native边界;zlib是DEFLATE/gzip的事实标准开源实现(zlib许可,捆绑分发);包形态沿用net库先例(`gzip.n`门面+`nlang_gzip.dll`)。http集成:客户端缓冲档(`request()`)发送时若用户未自设`Accept-Encoding`则注入`gzip`并自动解压——注入发生在发送层而非`defaultHeaders`表,解压与注入绑定:用户显式设置即放弃自动解压(Go transport同构,调研结论18);流式档(`open()`)不注入、不自动解压——v1 gzip仅缓冲(流式gzip见第一节递延裁定),流式路径发一个自己消费不了的编码是自挖坑,流式默认identity(RFC 9110:identity恒可接受),用户显式设置Accept-Encoding则字节原样交调用者;服务器`HttpResponse.compressBody=true`且客户端接受gzip时对缓冲body压缩,加`Content-Encoding: gzip`与`Vary: Accept-Encoding`;流式body不自动压缩(handler可自行预压缩)。 | 手写deflate易错且慢,不符合「采用主流开源库实现」裁定;解压是健壮性刚需(对端普遍gzip),压缩是便利面,同一native一并覆盖;DLL闭包/安装器细节归实施计划。 |
| D22 | 版本划分按用户裁定(2026-11-07):0.13.3=lambda/闭包(另行)、0.13.4=string增强+io.base64+system时钟+datetime+url、0.13.5=http+gzip、0.13.6=web框架(另行)。各版本公开面是最终面子集,后版只增不改。 | 用户裁定;datetime入0.13.4因它是通用基础设施、与string/base64同批,且0.13.5的http是其消费者;gzip入0.13.5因http是其唯一近期消费者,native捆绑随消费者落地。 |
| D23 | 最小化语言内核(用户裁定2026-11-08):除语言本身需要的基础设施和指令要内建于nvm外,需在C++实现的功能一律放独立库(`nlang_<ns>.dll`,或捆绑第三方开源库);VM内建注册仅限有明确理由——vm本身依赖,或极致性能优化。本项目应用:`system`时钟两native→新建`nlang_system.dll`(取代v2.9「VM内建注册、零新DLL」方案;`monoMs`经宿主nowMs回调实现,与调度器截止时间同时钟域,`wallMs`经`std::chrono::system_clock`);gzip→`nlang_gzip.dll`捆绑zlib(已合此裁定);url/datetime/http/io.base64纯脚本(零C++);string三方法保持intrinsic——用户裁定明确理由:string是语言基本类型,其成员操作属内建于nvm的语言基础设施(D4)。既有VM内建函数族(string/list/dict等原语操作)属语言核心运行时基础设施,按本裁定不重构外迁。 | 内核最小化使C++功能可独立构建、分发、调试与替换,与math/io/fs/net既有形态统一;内建例外须有明确理由,防止内核随功能膨胀;string例外与system时钟的形态均经用户裁定(2026-11-08)。 |

---

## 五、脚本层API面

以下为声明草案(签名细节由实施计划钉死;`native`标记仅出现在需要处——url/datetime/http全为脚本实现,io.base64为脚本实现,system.wallMs/monoMs为native(nlang_system.dll,D20/D23),gzip为native)。

### stdlib/url.n

```nlang
// 异常:解析/解码失败一律抛 UrlException
const class UrlException : IOException { ... }

// 不可变URL值;parse是唯一构造入口
const class Url {
    public string scheme;      // 小写化后存储(RFC 3986 §6.2.3)
    public string userInfo;    // 未百分号解码的原样
    public string host;        // 小写化后存储
    public int port;           // 未给出为 -1
    public string path;
    public string query;       // null=无query、""=空query(URL-013);未百分号解码的原样
    public string fragment;    // null/""区分同query

    // 接受任意引用(绝对或相对,如服务器侧的origin-form "/p?q");语法错抛 UrlException。
    // maxBytes>0时限输入长度(服务器侧以头部预算传入),超限抛 UrlException。
    // http客户端要求绝对URL,发请求时对相对引用以 UrlException 拒绝。
    public static Url parse(string reference, int maxBytes = 0);
    public Url resolve(string ref);                // 按 RFC 3986 §5 一般语法解析
    public string toString();                      // 重组(幂等)
    public bool isAbsolute();                      // scheme 非空
    public Dict<string, List<string>> queryParams(); // 解码后的表单编码对;重复键成列表
    public static string encodeQuery(Dict<string, List<string>> params); // queryParams 的逆向构造(URL-006 往返)
}

public string percentEncode(string s);   // RFC 3986 unreserved 之外全编码(含 `/`);契约为单一组件值——作 path 段或 query 值合法,整路径须逐段编码后以 / 拼接
public string percentDecode(string s);   // 畸形转义抛 UrlException
public string formEncode(string s);      // application/x-www-form-urlencoded(空格为+)
public string formDecode(string s);
```

### stdlib/datetime.n(新增,D20)

```nlang
const class DateTimeException : Exception { ... }

// 不可变日期时间值,UTC 基准
const class DateTime {
    public int year;  public int month;  public int day;
    public int hour;  public int minute; public int second;  public int millis;

    public static DateTime now();                      // UTC 当前时间(经 system.wallMs 墙钟 native,D20)
    public static DateTime fromUnixMillis(long millis); // epoch 毫秒超 int32 值域(2026 年≈1.77×10¹²),一律 long
    public long toUnixMillis();
    public DateTime addMillis(long millis);            // 日历运算(days_from_civil 算法)
    public int dayOfWeek();                            // 0=周日..6=周六(http RFC 1123 `Date` 用)
    public string toString();                          // ISO 8601 "2026-11-07T12:00:00Z"
    public static DateTime parse(string iso);          // ISO 8601 UTC 形态(以 Z 结尾);畸形抛 DateTimeException
}
```

### stdlib/io.n(base64新增;时钟面移出至system.n)

```nlang
public string base64Encode(byte[] data);     // RFC 4648 标准字母表,带填充
public byte[] base64Decode(string s);        // 畸形抛异常
public string base64UrlEncode(byte[] data);  // URL 安全字母表,无填充(JWT 风格)
public byte[] base64UrlDecode(string s);
```

### stdlib/system.n(新增,时钟归属裁定2026-11-08,D20)

```nlang
native long wallMs();   // 墙钟:unix 毫秒,等价 CLOCK_REALTIME(D20)
native long monoMs();   // 单调钟:自 io.nowMs 迁入并改名(只用于间隔/超时/定时器;不向下兼容,D20)
```

### stdlib/gzip.n(新增,D21)

```nlang
const class GzipException : IOException { ... }

public byte[] compress(byte[] data);     // gzip(RFC 1952),native zlib
public byte[] decompress(byte[] data);   // 畸形/截断抛 GzipException
```

### stdlib/http.n

```nlang
const class HttpException : IOException { ... }

// 大小写不敏感的头部表(键存小写,保留原样供写出)
class HttpHeaders {
    public string get(string name);                 // 首个值,无则 null
    public List<string> getAll(string name);
    public void set(string name, string value);     // 替换全部同名
    public void add(string name, string value);     // 追加
    public void remove(string name);
    public bool has(string name);
    public List<string> names();                    // 去重后的原名列表
}

// 分帧感知body读取器:服务器请求体、客户端流式响应体共用(D5)
class HttpBodyReader {
    public int read(byte[] buf, int timeoutMs = 0); // 返回读得字节数,末尾返回 -1;服务器侧读受 bodyTimeoutMs(D7)
    public byte[] readAll();                        // 读到末尾;服务器侧超 maxBodyBytes 抛 HttpException(服务器按 D5 映射 413)
    public string bodyText();                       // readAll 后按 UTF-8 解码
    public void close();                            // 复用纪律见 D19:客户端耗尽后 close→回池,未耗尽 close→关连接(取消);服务器侧请求体由连接循环排干复用
}

class HttpRequest {
    public string method;
    public Url url;
    public string version;                          // "HTTP/1.1"
    public HttpHeaders headers;
    public byte[] body;                             // 客户端:缓冲请求体(与 bodyChannel 互斥)
    public Channel<byte[]> bodyChannel;             // 客户端:流式上传(chunked,D16)
    public HttpBodyReader bodyReader;               // 服务器:分帧感知请求体(D5)
    public string bodyText();                       // 便捷:bodyReader.readAll 解码(服务器侧)
}

class HttpResponse {
    public int statusCode;
    public string reasonPhrase;
    public string version;
    public HttpHeaders headers;
    public byte[] body;                             // 缓冲body(与 bodyChannel 互斥)
    public Channel<byte[]> bodyChannel;             // 服务器:流式响应(chunked+背压,D5/D18)
    public HttpBodyReader bodyReader;               // 客户端 open():流式响应body读取器(D5)
    public string bodyText();
    public Url requestUrl;                          // 重定向后的最终请求URL
    public bool compressBody = false;               // 服务器:客户端接受gzip时对缓冲body压缩(D21)
}

class HttpClient {
    public int connectTimeoutMs = 10000;            // 连接阶段(D15)
    public int readTimeoutMs = 10000;               // 每次读间隔;0=不限(SSE 长连接)
    public int callTimeoutMs = 0;                   // 整请求含重定向;0=不限
    public int maxRedirects = 10;
    public int maxConnsPerHost = 0;                 // 每主机活跃连接上限;0=不限;设值时池满借出阻塞至归还或 callTimeoutMs 超时(=0 无限等待,Go同构,D8)
    public HttpHeaders defaultHeaders;              // 预置 User-Agent: nlang-http/1.1;请求自带头部优先,默认只补缺;Accept-Encoding: gzip 不在此表——由缓冲档发送时注入(用户已自设则不注入,解压与注入绑定;open() 不注入,D21)
    public HttpResponse request(HttpRequest req);   // 缓冲档:内部 readAll,自动解压 gzip(D21)
    public HttpResponse open(HttpRequest req);      // 流式档:bodyReader 交调用者,close 纪律 D19;不自动解压、不注入 Accept-Encoding(D21)
    public void close();                            // 关闭全部连接:空闲直接关,在途调用经SocketException失败(取消手段,HTTP-CLI-011)
}

class HttpServer {
    // start/serve 前可配;启动后只读
    public int maxHeaderBytes = 65536;              // 超限 431(D9)
    public int maxHeaders = 100;                    // 超限 431(D9)
    public int maxBodyBytes = 16777216;             // 超限 413(D5)
    public int keepAliveTimeoutMs = 30000;          // 请求间空闲(D7)
    public int headerTimeoutMs = 10000;             // 头部阶段总时限,Slowloris(D7)
    public int bodyTimeoutMs = 10000;               // body 阶段每次读间隔,慢灌/半开(D7)
    public int writeTimeoutMs = 10000;              // 响应写出分段时限,半开推送(D7)
    public int maxConnections = 0;                  // 0=不限;超限 503(D14)
    public int maxRequestsPerConnection = 0;        // 0=不限;超限末响应 Connection: close(D14)

    public void start(string host, int port,
        func<HttpResponse, HttpRequest> handler);   // port 0 支持;非阻塞(D14)
    public void serve(string host, int port,
        func<HttpResponse, HttpRequest> handler);   // 阻塞入口(D14)
    public string localAddress();
    public int localPort();
    public void close();
}

// 共享默认客户端的便捷档(缓冲;timeoutMs=整请求上限,0=用默认)
public HttpResponse get(string url, int timeoutMs = 0);
public HttpResponse post(string url, string body,
    string contentType = "text/plain; charset=utf-8", int timeoutMs = 0);
public HttpResponse put(string url, string body,
    string contentType = "text/plain; charset=utf-8", int timeoutMs = 0);
public HttpResponse del(string url, int timeoutMs = 0);
public HttpResponse request(HttpRequest req, int timeoutMs = 0);
```

### stdlib/lang.n(string方法面新增,D4)

```nlang
public native int indexOf(string sub, int from);   // 从字节偏移 from 起找;from<0 按 0,from>length 返回 -1;-1 无命中
public native int toInt(int radix);                // 严格整串按进制解析(2..36),允许前导 +/-,畸形/溢出抛 Exception
public native bool equalsIgnoreCase(string other); // ASCII 字母大小写不敏感
```

---

## 六、基础设施盘点(整体架构指示的落实)

按「消费者驱动」逐一裁定,避免臆测泛化。用户裁定(2026-11-08):检验和改进nlang本身的基础设施是开发本系列库的主要目的之一——故基础设施缺口的改进(标准库/语言/虚拟机/工具链)属本项目正式交付范围而非附带工作;开发过程中暴露的基础设施问题(含能力缺失、诊断不清、工具链障碍)应回本项目内解决,不以库内绕行长期掩盖:

| 缺口 | 消费者 | 裁定 |
| --- | --- | --- |
| `indexOf(t, from)`、`toInt(radix)`、`equalsIgnoreCase` | url组件边界、http头部扫描、百分号/chunked解码、头部名比较 | 补入`string`(D4);语义按Java先例钉死。 |
| base64编解码 | http认证(Basic)、chunked无关但通用;用户面 | 补入`io`包四函数;位置与变体面按Java/Go/Python先例(调研结论11)。 |
| 日期/时间 | http `Date`/`Expires`头、用户面 | 专门`datetime`包(D20,用户裁定);纯nlang+`system.wallMs`(新增墙钟native);v1 UTC。 |
| gzip压缩/解压 | http `Content-Encoding`(解压是健壮性刚需) | `gzip`包,native zlib(D21,用户裁定)。 |
| 通用tokenizer/Scanner | 同上扫描需求 | 不补:indexOf+substring+循环覆盖全部已知消费者,再加抽象属臆测泛化(D4)。 |
| `Channel.recv`超时 | 流式pump | 不补:取消语义由生产者`finally`关通道+`ChannelClosedException`覆盖;`recv`停摆(生产者阻塞在他处到不了finally)以生产者契约兜底——违约属调用方缺陷,每违约请求至多泄漏一个连接,`maxConnections`为可选部署上限(D18),不为此扩并发包面;recv超时属并发包独立演进议题。 |
| MIME content-type嗅探 | web框架静态文件(0.13.6) | 递延至消费者立项。 |
| SSE helper(event-stream编码) | web框架(0.13.6) | 递延;http层不理解SSE语义(http_verify §6),本项目以示例演示手工framing。 |
| 本地时区/日历库扩展 | 用户面 | datetime roadmap后续(D20)。 |
| 时钟函数归属 | datetime.now、net/concurrent超时截止域、http超时 | 移出io新建`system`包(时钟归属裁定,调研结论20);环境变量、命令行参数、进程信息为`system`后续居民,本项目无消费者不预支。 |

---

## 七、native层结构

- url.n/datetime.n/http.n:零新增native、零新增DLL;新建`system.n`承载墙钟native`wallMs`与自io.n迁入改名的`monoMs`(io.n删除`nowMs`;用户裁定2026-11-07/11-08——改进标准库/VM行为受支持,只要需求合理;不合理名称与归属不因现有实现维持),二者由新建`nlang_system.dll`经NativeHost C ABI提供(沿用math/io/fs/net先例:`monoMs`经宿主nowMs回调实现——与parkForIo/调度器截止时间同时钟域,`wallMs`经`std::chrono::system_clock`;用户裁定2026-11-08——时钟实现形态与既有native模块统一,D23),datetime经`import system;`消费(路径见D2)。`stdlib/net.n`内部native`_nowMs`(nlang_net.dll注册、计入钉数net 54)是内部面非用户可见,不受时钟归属裁定波及,数目不变。
- string三方法为VM内建注册(预置包,与既有string natives同路径;用户裁定2026-11-08:string属语言基本类型,成员操作做成intrinsic,D4/D23),仅动`lang.n`声明与VM内建实现。
- gzip:native实现捆绑zlib(zlib许可),新增`nlang_gzip.dll`(包形态沿用net库先例);CMake集成、DLL闭包与安装器条目归实施计划。
- url.n/system.n/http.n/datetime.n/gzip.n经stdlib GLOB自动进npkg打包与安装,脚本面无构建系统改动(纯脚本包路径由计划探针验证);`nlang_system.dll`需构建系统改动(CMake模块清单与安装清单,归计划)。

---

## 八、关键数据流

**服务器请求生命周期**:`accept`→活跃连接数≥`maxConnections`则回503+关连接→新连接`setNoDelay(true)`+`setKeepAlive(true)`(D7)→`thread.spawn(连接run)`→连接线程循环:缓冲读取器在`headerTimeoutMs`内读请求行+头部(≤maxHeaderBytes→431、条数≤maxHeaders→431、严格CRLF、裸CR/LF/控制字符拒绝、obs-fold拒绝、CL∧TE拒绝、TE严格、Host校验、方法token校验、未知版本505)→`Expect: 100-continue`则先发100→构造`bodyReader`(按CL/chunked/EOF分帧;读受`bodyTimeoutMs`,D7)→调handler(异常→500+关连接)→handler未读完body则有限排干(≤256KB)再复用,超限关连接→写出响应:`compressBody`且客户端接受gzip则先压缩缓冲body再定头部(D21,压缩改变Content-Length)→缓冲body→Content-Length;`bodyChannel`→chunked泵(逐块即发=flush语义(D18),send阻塞=背压;写受`writeTimeoutMs`分段时限,D7;写失败→close通道→生产者经`ChannelClosedException`停止)→HEAD/304跳过body写出(handler给body或bodyChannel亦只写头部):HEAD响应必须保留Content-Length(等于GET应发实体长度,HTTP-SRV-014,RFC 9110 §8.6);304省略Content-Length(RFC 9110允许携带但须等于200实体长度,主流服务器省略);204无body且不带Content-Length(RFC 9110 §8.6)→keep-alive则循环(请求间空闲≤keepAliveTimeoutMs;累计请求数≥`maxRequestsPerConnection`则末响应`Connection: close`),否则关闭。

**客户端请求生命周期**:自由函数/方法→`Url.parse`(失败UrlException;非`http` scheme在组装时抛HttpException拒绝,D17)→组装HttpRequest(合入defaultHeaders——请求自带头部优先、默认只补缺;缓冲档发送时注入Accept-Encoding: gzip(用户已自设则不注入),流式档不注入,D21)→池借出(host:port键,`isClosed()`探测,过期/已关丢弃新开;`maxConnsPerHost`设值时池满阻塞等待归还,超`callTimeoutMs`抛TimeoutException,D8)→写请求(缓冲body或chunked泵`bodyChannel`)→读状态行(1xx跳过续读)→读头部→缓冲档readAll,若本次请求由库注入Accept-Encoding且响应Content-Encoding: gzip则解压并剥离Content-Encoding/Content-Length(解压与注入绑定,D21;用户自设Accept-Encoding则字节原样交回);流式档交回`bodyReader`,不自动解压(D21)→3xx且允许则按D12改写请求重试(计数≤maxRedirects,超限HttpException;307/308携`bodyChannel`不可重放→HttpException)→成功则按D19纪律归还(耗尽后close回池;未耗尽close即关连接——取消载体)→返回HttpResponse(带requestUrl)。

**解析器可测性**:请求/响应解析器设计为对`byte[]`缓冲的纯函数(输入缓冲+已读长度,输出消息+消耗量),不经网络即可单测加固规则;网络侧只是其上的缓冲读取循环。半包/粘包由「缓冲不足→返回需要更多」的纯函数契约天然覆盖(http_verify红线3)。

---

## 九、测试策略(采纳`docs/ref/http_verify.md`用例编号与分级)

分级约定:**B级=必过e2e**,M级=入e2e或单测必覆盖,m级=择要覆盖。三条红线(增量解析半包/粘包、背压、取消传播)各有专项用例,不允许以「单次绿」交差。与参考标准的偏离集中在本节末「适配清单」,B级判定以该清单为准。参考标准本身经考证不合理的用例可直接修订(用户裁定2026-11-07),修订理由登记于参考文档内。

1. **url单测**(e2e .n,纯计算):URL-001…013逐条;URL-B01…B07全过;URL-S01…S03(安全:path穿越范围、CRLF注入、长度上限);URL-P01…P04(纯函数/错误可区分/并发安全/性能基线);RFC 3986附录B测试向量逐组件断言;percent/form编解码往返;resolve一般语法样例集(RFC 3986 §5.2参考解析向量,URL-003判定见适配清单);queryParams重复键/加号/百分号交互。
2. **基础设施纯计算单测**(0.13.4):string方法——`indexOf(t, from)`起点边界(from=length、from=-1、命中/未命中)、`toInt(radix)`十六进制/溢出/畸形/负号、`equalsIgnoreCase`ASCII边界与非字母;datetime——epoch毫秒↔日历往返(闰年/世纪年/负epoch)、`addMillis`跨月跨年、`dayOfWeek`已知日期断言(RFC 1123 `Date`消费者)、ISO 8601 `toString`/`parse`往返+畸形抛`DateTimeException`、`now()`值域合理(epoch毫秒晚于2020年,与`system.wallMs`读数一致);base64——四函数往返(空串/1字节/长度3n±1填充位)、畸形输入抛异常、URL安全字母表无`+/`无填充。
3. **http分帧单测**(纯函数驱动byte[]):HTTP-SRV-001…020逐条(505未知版本、方法token校验、Host必需、CL∧TE拒绝、chunked解码、OWS容错、obs-fold拒绝、100-continue/417、HEAD/304抑制body、HTTP/1.0默认close);HTTP-CHK-001…007(chunked编解码往返、非法chunk size→400、`0\r\n\r\n`精确匹配、chunk extension忽略、trailer丢弃、编码器输出格式);HTTP-S01…S05(431头部超限/条数超限、Slowloris头部/body时限(bodyTimeoutMs)、写侧CRLF注入拒绝、控制字符/`\0`);413;半包/粘包增量解析(红线1);gzip直接单测(0.13.5,http_verify无gzip用例故自拟)——`compress`/`decompress`往返(空/1字节/大缓冲/不可压缩数据)、畸形与截断输入抛`GzipException`。
4. **服务器e2e**:HTTP-CONN-001…009(keep-alive复用计数、maxConnections→503、maxRequestsPerConnection、空闲/头部/body/write四时限各自断连用例(bodyTimeoutMs慢body细水灌断连、writeTimeoutMs半开/停滞客户端写出断连)、Upgrade可见不握手、Connection多token、半关闭行为明确)。
5. **客户端e2e**(回环自足,port 0):HTTP-CLI-001…011逐条(含CLI-004超时分离、CLI-008 chunked上传、CLI-010出错连接不回池、CLI-011取消传播:流式reader中途close→服务器观测连接关闭、阻塞调用期间`close()`→`SocketException`且socket关闭);池isClosed探测;重定向链(301→GET、307保持方法、307+`bodyChannel`不可重放抛HttpException、跨主机剥离Authorization、超限抛HttpException);1xx跳过;4xx/5xx不抛;gzip自动解压往返与用户自设Accept-Encoding退出(字节原样不解压,D21)。
6. **高并发**:HTTP-HC-001…020(连接上限503、空闲回收、半开读写超时回收(HC-004:读写时限快速发现+setKeepAlive兜底)、`maxConnsPerHost`池满排队(归还唤醒成功与callTimeoutMs超时两路径)、缓冲行为、流式峰值内存、背压、取消传播、取消后资源回收、同host复用率≥90%;HC-003/009/016/019/020见适配清单);http_conc_bench示例(1000连接×N请求持久复用,net_conc_bench先例);并发族本地压测百次+断言构建冒烟(单次绿不算数)。
7. **SSE专项**:SSE-001/002/004在http层验证(chunked流式往返、客户端断开→pump写失败→生产者停止、慢消费者背压);SSE-003心跳与SSE-008语义归属经`http_sse.n`示例演示(手工framing+`:`心跳注释行,http.n源码无SSE格式拼接;SSE-003为B级,示例判定见适配清单);SSE-005/006规模按本机容量适配(见适配清单);SSE-007任意请求头透传经客户端头部往返用例覆盖(M级)。
8. **性能基准**:http_verify §7基线(conc吞吐、解析吞吐)。
9. **混沌**:http_verify §8(随机断连、半关闭、畸形字节流注入)。
10. **探针**:纯nlang包(零native)经npkg打包后`import url;`可加载可调用——实施计划第一个任务验证。

**适配清单**(与`http_verify.md`验收标准的偏离,各有独立理由;B级判定以本清单为准):

| 用例 | 适配 | 理由 |
| --- | --- | --- |
| URL-003 | B级期望「与浏览器`new URL(href, base)`行为一致」按RFC 3986 §5.2一般语法参考解析向量判定 | D3裁定RFC 3986路线、排除WHATWG特殊规则(调研结论2);用例所列样例两类标准一致,WHATWG分歧(特殊scheme、不透明路径细节)不在判定范围 |
| URL-004 | 统一保守百分号编码,不按组件preserve set分集 | 契约为单一组件值:组件内过度编码往返正确;`/`→`%2F`在path段合法但不等价于`/`(RFC 3986 §2.2/§6.2.3),整路径须逐段编码;URL-B02/B03的整路径期望经逐段编码+`/`拼接满足(D3,调研结论9) |
| URL-B01 | 空串引用抛UrlException | 空引用无消费者:服务器origin-form至少`/`,客户端要求绝对URL |
| URL-P02 | 单一UrlException类,message区分原因 | Go/Java单一异常类型先例;M级以message断言覆盖 |
| HTTP-S01 | 单行长度与chunk size不设独立上限 | 单行超限经总预算触发431;拉式reader按调用方缓冲读取,声称长度不决定分配(HTTP-CHK-002无OOM满足) |
| HTTP-S06/HTTP-CLI-009 | 自动跟随重定向、自动解压gzip | 用户目标「方便的客户端API」(D12/D21,Go/requests同默认);`maxRedirects=0`与显式设Accept-Encoding均可关 |
| HTTP-SRV-002 | method按合法token校验,不限全大写 | RFC 9110方法是大小写敏感token,扩展方法可含小写;仅大写严于协议会误拒合法扩展方法(D9) |
| HTTP-SRV-015 | 拆分判定:OWS(冒号后/值前后空白)按RFC 9112 §5.1剥离不致解析失败;冒号前空白`Key :Value`按畸形头部拒绝(400) | 原期望「按 RFC 规整,不因空白导致解析失败」自相矛盾——RFC 9112 field-line语法(`field-name ":" OWS field-value OWS`)冒号前无空白,Go textproto与llhttp均拒绝;参考用例已按用户裁定修订(2026-11-07,修订登记于参考文档) |
| HTTP-SRV-020 | TRACE/CONNECT不预过滤,到达handler | 协议层不预设业务策略,handler回405(与D6同因) |
| HTTP-HC-003 | 带deadline的虚拟线程读替代时间轮 | VM调度器统一管理deadline,空闲连接是parked虚拟线程,CPU不随连接数线性上涨(D7) |
| HTTP-HC-009 | 24h长稳以本地百次压测替代 | CI容量;并发族「单次绿不算数」纪律不变 |
| HTTP-HC-016 | 默认活跃连接不设上限、无排队(Go默认不限先例);`maxConnsPerHost`设值时池满排队、超时抛TimeoutException | B级排队行为可表达可测(D8) |
| HTTP-HC-019/020 | v1无指标面 | 观测经示例与测试(复用率经localPort/连接计数可观测);指标面随消费者出现另行设计 |
| HTTP-CONN-009 | Upgrade对handler可见、库不握手 | 等效拒绝且非静默(D9) |
| SSE-003 | B级经`http_sse.n`示例往返判定:空闲期`:`心跳注释行逐块到达客户端、连接存续 | 心跳是用法形态非协议机制——http.n不理解SSE(D5/http_verify §6),心跳行即普通chunked数据块,机制由示例往返覆盖,库不预置SSE helper(与SSE-008同因) |
| SSE-005/006 | 万级长连接按1000连接适配 | 本机容量;与conc_bench同形态,百次压测纪律不变 |

---

## 十、示例设计

| 示例 | 内容 | 退出码/断言 |
| --- | --- | --- |
| `http_hello.n` | 服务器(port 0)+客户端同进程:GET往返、echo POST、keep-alive两次请求 | 0,stdout含断言文本 |
| `http_conc_bench.n` | 1000虚拟线程并发GET持久连接复用,报请求总数与耗时 | 0,stdout含完成标记 |
| `http_sse.n` | 服务器`bodyChannel`手工event framing(`data: …\n\n`+`:`心跳注释行),生产者线程`finally`关通道;客户端`open()`逐块接收;客户端提前断开→服务器写失败→生产者经`ChannelClosedException`停止 | 0,stdout含取消传播断言 |
| `url_tour.n` | parse/resolve/percentEncode/queryParams往返展示 | 0 |

均登记`examples_manifest.txt`,文档围栏示例真实编译运行。

---

## 十一、集成点与文档同步

1. 新增:`stdlib/system.n`、`stdlib/url.n`、`stdlib/datetime.n`、`stdlib/http.n`、`stdlib/gzip.n`(GLOB自动打包);修改:`stdlib/lang.n`三方法声明+VM内建实现、`stdlib/io.n`base64四函数+删除`nowMs`(调用点同步:`examples/net_conc_bench.n`、`tools/net_bench/net_bench_10k.n`(不入e2e manifest,删`nowMs`后静默坏死、CI不报)、e2e`stdlib_io_park_guard/stdlib_io_park_now/stdlib_net_ping_stress`,`io.nowMs()`→`system.monoMs()`;`tests/test_nide/test_mainwindow.cpp`的io补全弹窗计数钉数19随面变化改22(删nowMs、增base64四函数)并同步其枚举注释);native:新建`nlang_system.dll`提供`system.wallMs`/`monoMs`(CMake模块清单、安装清单归计划)、gzip捆绑zlib(CMake、DLL闭包、安装器归计划)。
2. `test_library_index.cpp`钉数两级:267→276(0.13.4:url自由函数4、io.base64四函数、system.wallMs 1;`io.nowMs`迁入system改名`monoMs`为净零——io 19→22、system 2,总数只随新增增长)→283(0.13.5:http自由函数5(get/post/put/del/request)、gzip自由函数2);string方法与类静态/成员方法不计;计划核实(代码侧注释「266」为陈旧值,重算时顺带修正);符号索引测试同步;IDE树全量ctest收尾(编译器/预置包变更门禁)。
3. 手册:0.13.4新增`library/15-system.md`(插于14-io之后,现15-20章顺延为16-21)、`library/22-url.md`、`library/23-datetime.md`,格式章21/22/23→24/25/26;0.13.5新增`library/24-http.md`、`library/25-gzip.md`,格式章再顺延为27/28/29;**引用同步规则**(取代手写计数):每轮重编号,对当轮被顺延各章的全部节号引用(`§N.M`、`第N章`、`chapter N`三形态——zh树用「第N章」,en树`chapter`/`Chapter`大小写混用,grep须不区分大小写)与文件名链接随轮顺延,含被顺延章自身的内部节号自引用与章文件自身改名——0.13.4轮波及现15-23章(string方法章§16.2→§17.2及文件`16-string-methods.md`→`17-string-methods.md`、库异常章§19→§20及`19-library-exceptions.md`→`20-library-exceptions.md`、格式章21/22/23→24/25/26及其内部`§21.x`/`§22.x`/`§23.x`自引用),0.13.5轮波及格式章24-26→27-29;由计划在`docs/user_manual/zh/specification/`与`en/specification/`两树按四形态(`§15`–`§29`、`第15章`–`第29章`、`chapter 15`–`chapter 29`不区分大小写、被改名文件名)grep列全——引用面散布数十文件(计数随形态口径而变,spec不钉死),手写清单已两轮漏项,且陈旧节号(含「第N章」纯文本形态)链接有效、linkcheck测不出;0.13.4同步`14-io`删除时钟小节;重编号后的`abi`格式章宿主函数表`nowMs`回调行保持原样(C ABI时钟域名,与脚本命名空间无关,`wallMs`/`monoMs`是脚本层native不入该表;可加注说明`system.monoMs`即由该nowMs回调实现、二者同时钟域),停泊截止域措辞无需改(中英成对);§17.2方法表增三方法;§20库异常两轮增补:0.13.4增UrlException/DateTimeException,0.13.5增HttpException/GzipException;`index.md`目录与版本行;中英成对。
4. `CHANGELOG.md`+`VERSION`→0.13.4、0.13.5两级;`docs/roadmap.md`按版本划分表增行。
5. e2e/示例manifest登记;`check_no_builtin_stdlib.py`正控io期望14→13(删`nowMs`;base64为脚本实现不增native声明),math/fs不变;为`system.n`增设正控(2条native声明——裁定:与math/io/fs同列,保持守卫对预置包native面的覆盖;url/datetime/http纯脚本无native,无需正控)。
6. 手册围栏与`manual_sync_check.py`基线两轮重锚。

---

## 十二、验收标准

1. url/datetime/http全部为纯nlang源,`stdlib/`下无对应DLL(gzip、system除外);`import url;`/`import datetime;`/`import http;`/`import gzip;`在npkg安装形态下可用(探针用例入e2e)。
2. `docs/ref/http_verify.md`全部B级用例通过(按第九节适配清单判定);M级用例覆盖;三条红线(增量解析、背压、取消传播)各有专项用例通过。
3. RFC 3986附录B向量全过;加固用例全过(431/413/417/505/503/100时序、服务器四时限断连)。
4. 客户端:默认重定向10、跨主机剥离凭据、池复用可观测、客户端三级超时与服务器四时限语义可观测、gzip自动解压往返。
5. SSE回环:服务器chunked推流→客户端逐块收;客户端断开→服务器写失败→生产者停止;慢消费者背压可观测。
6. http_conc_bench 1000连接并发通过,且本地压测(无gc-stress形态)百次无flake。
7. 七门禁+风格门禁全绿;Debug断言构建冒烟通过。
8. 手册新章节中英齐备,格式章两轮重编号后nav-coverage与围栏门禁绿。

---

## 十三、风险与开放问题

| 风险 | 缓解 |
| --- | --- |
| 纯nlang整包加载路径无先例(npkg/loader交互) | 实施计划第一任务即探针;若受阻,回退方案是包内加一个占位native(改动小,需重裁D2)。 |
| `func<HttpResponse, HttpRequest>`类类型实参支持未实证 | 探针任务一并验证;thread.n `func<T>`类实参先例支持预期。 |
| 流式泵死锁(生产者泄漏通道) | D18纪律:生产者`finally`关通道;背压用例(SSE-004)与取消用例(SSE-002)入e2e;bounded通道只约束`send`方向,`recv`停摆以生产者契约兜底(违约至多泄漏一连接),`maxConnections`为可选部署上限(D18/§六)。 |
| zlib捆绑引入构建/分发面(CMake、DLL闭包、安装器) | 沿用net库native包先例的打包路径;计划列全DLL闭包清单;gzip失败不影响url/http主线。 |
| 脚本层解析性能未知 | conc基准给出数据;D2逃生口(native化不改API)。 |
| datetime日历运算边界(闰年、世纪、负epoch) | days_from_civil算法采用主流已验证实现;测试向量含边界年。 |
| 客户端/服务器空闲竞态(D8) | isClosed探测+短空闲超时;残余竞态窗表现为SocketException,语义明确。 |
| 两轮重编号遗漏被顺延章的节号/文件名引用(陈旧节号链接有效,linkcheck/nav-coverage测不出) | §十一.3引用同步规则,计划按四形态grep列全(`§N.M`/`第N章`/`chapter N`不区分大小写/文件名);manual_sync_check+nav-coverage围栏兜底。 |
| web框架何时立项 | 依赖lambda闭包(v0.13.6);roadmap按用户划分登记,本项目不预支其设计。 |

---

## 修订记录

- 2026-11-08(v2.10):并入用户内核最小化裁定(2026-11-08,四条合并:除语言本身需要的基础设施和指令要内建于nvm外C++功能一律放独立库;最小化语言内核;spec不合理处可调整;内建注册仅限vm本身依赖或极致性能优化等明确理由)。应用:①`system.wallMs`/`monoMs`改由新建`nlang_system.dll`经NativeHost C ABI提供(用户裁定「system.monoMs/wallMs也应该是外部dll,和原来的方式统一」「wallMs不属于语言的一部分,无其他理由,不需要做成intrinsic,做成普通native就可以」,取代v2.9「VM内建注册、零新DLL」方案;`monoMs`经宿主nowMs回调实现、与调度器截止时间同时钟域,`wallMs`经`std::chrono::system_clock`;波及输入节、状态行、D2回退路径、D20、§五前言、§七、§十一.1/3、§十二.1,新增D23);②string三方法形态经用户裁定保持String面类方法+intrinsic(「string本身就是语言的一部分,需要做成intrinsic」,归`lang`库,属D23「vm本身依赖」明确例外——string接收者是字符串池索引而非对象,成员调用只经内建函数表分派;不做包自由函数;D4、§七);③既有VM内建函数族属语言核心运行时基础设施,不重构外迁(D23)。
- 2026-11-08(v2.9):并入第九轮审核(0严重/0重要/4次要,经实测核实,审核判决无中级及以上问题,循环收束):Minor——§一.3「v1做」配套清单补`system`时钟包与`io.nowMs`移出改名(R9-01)、`http_verify`红线括注编号对齐参考文档§12.3实际编号(半包/粘包=红线3、取消传播=红线2,原两处错位,R9-02)、事实2的`04-types/string.md`行号改zh树:95(原:166仅en树成立,中文文档默认zh读法落空,R9-03)、事实1的`write`简写补byte[]重载可选`count`参数说明(net.n:297,防按位置绑定误入,R9-04)。
- 2026-11-08(v2.8):并入第八轮审核(0严重/1重要/2次要,经实测核实):Important——v2.7引用同步规则的grep形态清单本身失实:zh树节号引用主形态是「第N章」(实测`第15章`–`第23章`共21处散布10文件,含`11-conventions.md`——该文件对15-23章的引用全部是「第N章」形态,零`§`/`chapter`形态)且en树有`Chapter 19`大写形态,按v2.7两形态清单列全会静默漏掉10+文件,陈旧「第N章」纯文本引用linkcheck/nav-coverage同样测不出——§十一.3形态清单补为`§N.M`/`第N章`/`chapter N`(大小写不敏感)三形态、计划grep改四形态口径,事实10/§十三随轮同步(R8-01)。Minor——清除v2.7残留的手写计数「36文件」(两处;计数随口径而变,审核者宽口径实测66文件,钉数本身即R7-01要根除的失败模式,R8-02)、版本划分表0.13.4行补「§20增补」(与0.13.5行对称,§20库异常章0.13.4即增UrlException/DateTimeException,R8-03)。
- 2026-11-08(v2.7):并入第七轮审核(0严重/1重要/1次要,经实测核实):Important——手册重编号级联的引用面手写计数失实(实测散布36文件,含`chapter N`形态、`10-concurrency`/`11-conventions`/`04-types/*`/`08-exceptions`/`reference/known-limitations.md`等指向15-23章的引用、格式章自身内部节号自引用、两章改名后的文件名链接;陈旧节号链接有效,linkcheck/nav-coverage测不出,手写清单已两轮漏项),事实10/§十一.3/§十三风险表改为规则式同步——计划按`§N.M`/`chapter N`/文件名三形态在中英两树grep列全(R7-01)。Minor——§九补gzip直接单测行(往返/畸形/截断→`GzipException`,http_verify无gzip用例,R7-02)。
- 2026-11-08(v2.6):并入第六轮审核(0严重/3重要/4次要,均经实测核实):Important——`check_no_builtin_stdlib.py`正控io期望14→13并为`system.n`增设正控(R6-01)、调用点清单补`tools/net_bench/net_bench_10k.n`(不入manifest致CI不报,R6-02)、`abi`格式章指令改为「宿主函数表`nowMs`回调行保持原样」(原「内建表迁system增wallMs/monoMs」会错改C ABI契约,与D20「无ABI变更」自相矛盾,R6-03)。Minor——§六datetime行「既有clock native」旧表述清扫(R6-04)、`net.n`内部`_nowMs`不受归属裁定波及声明入§七(R6-05)、§16.2/§19交叉引用文件列全(实测5处,含审核者未列的`20-network/index.md`,及两章文件自身改名,R6-06)、§八压缩次序改先压缩后定Content-Length(R6-07);另§十一.2登记代码侧注释「266」陈旧值顺带修正。
- 2026-11-08(v2.5):并入用户时钟归属裁定——时间相关函数移出`io`包,新建`system`包承载(用户提议,经核实采纳:时钟非I/O设施,主流语言均不置于I/O模块,调研结论20;`datetime`已占日历niche故不名`time`)。`io.nowMs`迁入改名`system.monoMs`(单调钟),新增`system.wallMs`(墙钟,VM内建注册,零新DLL);`io.n`删除`nowMs`,调用点(`examples/net_conc_bench.n`、e2e三例)同步;`datetime.now()`经`import system;`消费。波及:输入节、事实2/9/10、调研结论20、D2/D20/D22、§五(io.n/system.n分块)、§六盘点行、§七、§九第2项、§十一第1/2/3/5项、版本划分表(0.13.4增`system`时钟包;手册改15-system新章插入致15-20→16-21顺延、url/datetime改22/23、格式章两轮改24-26/27-29、§16.2→§17.2、§19→§20)、状态行。索引钉总数不变(267→276→283,迁入改名净零)。并入第五轮审核(0严重/1重要/3次要):Important——D5排干上限与D19统一,`maxBodyBytes`仅在readAll处强制、排干上限为D19固定256KB(R5-01);Minor——§十一第1项补`test_mainwindow.cpp`io补全弹窗计数同步对象(19→22,R5-03)、v2.4修订记录墙钟native定名合并说明(R5-04);R5-02(`23-abi`陈旧指针)被v2.5手册条目改写消解。
- 2026-11-07(v2.4):并入第四轮架构师审核——Important:D19客户端close语义统一(未耗尽close=关连接即取消载体,排干复用仅归服务器侧请求体;D19/§五/§八,R4-01);`DateTime.now()`不能复用`io.nowMs`(steady_clock单调钟无法产出日历字段),新增io墙钟native(VM内建注册、零新DLL;经同日用户命名裁定定名`wallMs`,R4-02初稿曾用名`wallNowMs`;D2/D20/§五/§七/§十一,R4-02)。Minor:D18「默认8块」改为handler构造通道+推荐容量(R4-03);事实1补`setKeepAlive`(R4-04);§五readAll的413措辞改服务器侧映射(R4-05);HTTP-SRV-015拆分判定——冒号前空白按RFC 9112 §5.1拒绝、参考用例经裁定修订(D9/适配清单/参考文档,R4-06);URL-003 B级按RFC 3986 §5.2向量判定(§九/适配清单,R4-07)。自查补充:DateTime epoch毫秒接口`int`→`long`(epoch毫秒超int32值域);§九第2项补datetime/base64单测面(原缺覆盖);§五前言native标记说明补io时钟native。另按用户命名裁定:新增`io.wallMs()`(墙钟,等价CLOCK_REALTIME)、既有`io.nowMs()`改名`io.monoMs()`(单调钟,只用于间隔/超时/定时器;不合理名称不因现有实现维持,不向下兼容)——波及D2/D20/§五/§七/§九/§十一与调用点、手册14-io/23-abi(输入节登记裁定)。另登记用户裁定:改进标准库/语言/VM/工具链行为受支持,只要需求合理(输入节;D2/D20援引)。2026-11-08再登记:开发本系列库的主要目的之一是检验和改进nlang本身的基础设施(输入节/§六前言),全局重申所有功能不向下兼容、合理性优先(裁定原则)。
- 2026-11-07(v2.3):并入第三轮架构师审核——Important:§八解压改与注入绑定(与D21一致)并补用户自设Accept-Encoding退出用例(R3-01);HTTP-CLI-011取消载体明确——流式reader中途close即关连接、`close()`连同在途连接关闭使阻塞调用经`SocketException`失败(D19/§五/§九,R3-02)。Minor:`Channel`容量0=无界事实修正与D18容量下限≥1(R3-03);跨包native复用路径裁定`import io;`+探针+回退(D2/D20,R3-04);HEAD必须保留Content-Length、304省略(RFC 9110 §8.6/§15.4.5考证,HTTP-SRV-014,R3-05);D5之413限readAll路径、排干超限关连接(R3-06);池空闲参数定为v1固定常量(R3-07);非ASCII主机原样保留策略文档化(URL-010,R3-08);SSE-003适配行补入(R3-09);obs-fold引用修正RFC 9112 §5.2(R3-10)。另按用户裁定登记「参考标准不合理用例经考证可修订」(§九)。
- 2026-11-07(v2.2):并入第二轮架构师审核——Important:`maxConnsPerHost`等待语义补全(`callTimeoutMs=0`无限等待,Go wantConn队列同构,调研结论16)与排队两路径用例(D8/§五/§九);`Accept-Encoding: gzip`改由缓冲档发送时注入、解压与注入绑定(新增调研结论18,Go transport机制;D15/D21/§五/§八);pump`recv`停摆界定改以生产者契约为主、`maxConnections`降为可选部署上限(D18/§六/§十三);服务器四时限断连用例入§九/§十二。Minor:D9指向§八的HEAD/304/204规则、适配清单URL-004注明URL-B02/B03满足方式、§十二超时措辞、§八泵注明flush语义、SSE-007头部透传覆盖。
- 2026-11-07(v2.1):并入单轮架构师审核——Critical:流式档与默认Accept-Encoding/自动解压互斥,`open()`默认identity(D21/§五/§八);Important:服务器body/写出两阶段时限`bodyTimeoutMs`/`writeTimeoutMs`与setNoDelay/setKeepAlive(D7/§五/§八/§九),pump`recv`停摆界定修正(D18/§六/§十三),method token校验适配行(HTTP-SRV-002),percentEncode单一组件契约(D3/§五/适配清单URL-004),`maxConnsPerHost`旋钮保留(D8/§五/§九/适配清单HC-016),307/308携`bodyChannel`不可重放抛HttpException(D12/§八/§九);Minor:非http scheme拒绝(D17/§八),HEAD/304跳过body写出(§八),defaultHeaders合并优先级(D15/§八),§九第1项URL-S描述修正,索引钉两级267→275→282(§十一),§19异常按版本拆分(§十一),toInt(radix)异常类型对齐既有家族(D4/§五)。
- 2026-11-07(v2):并入用户裁定——流式/SSE(D5/D16/D18重写,删「流式body/chunked写出」两行递延)、版本划分0.13.4/0.13.5(D22与第一节版本划分表)、gzip采用主流开源库实现(D21)、日期时间专门标准库(D20);新增基础设施盘点节;D9按调研改为431/505/503并补obs-fold/OWS/控制字符/多token Connection/Upgrade透传/request-target三形式;D7拆分headerTimeoutMs(Go/Node先例);D15超时拆connect/read/call(OkHttp先例);D19连接复用纪律(Go 1.27先例);D14改实例start/serve并补maxConnections/maxRequestsPerConnection;§五API草案与裁定对齐(reader/channel成员、datetime/gzip/io.base64面);测试策略采纳http_verify用例编号与B/M/m分级并补适配清单(逐条裁定与参考标准的偏离,含204无Content-Length、query null/""区分、encodeQuery补齐等自审修正);§三补两轮调研出处。
- 2026-11-07(v1):初稿定稿(调研见第三节,勘察事实见第二节;D4按用户整体架构指示补string三方法)。

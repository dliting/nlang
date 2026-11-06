# net 网络库实施计划

> **给实施代理:** 必读子技能:用 superpowers:subagent-driven-development(推荐)或
> superpowers:executing-plans 逐任务实施本计划。步骤用复选框(`- [ ]`)跟踪。

**目标:** 按`docs/dev/net_library_design.md`实现nlang通用网络库`net`包:虚拟线程停泊
式高并发TCP/UDP/ICMP,含六个示例、en+zh手册与全套门禁。

**架构:** socket一律非阻塞;VM调度器的通用IO readiness设施(`IoPoller`+wepoll/epoll,
按fd通用,net为第一个消费者)收割就绪并唤醒`ParkedIo`停泊上下文(复刻既有定时器线程与
Channel两阶段停泊握手);网络实现住在VM**外**的`nlang_net` native模块(类第三方库,
不链接nlang_vm),经ABI v11 `nowMs`/`parkForIo`/`moduleState`/`wakeIoWaiters`/
`currentContextId`/`submitWork`获得通用IO停泊与原生工作线程池能力,其脚本面归io包
(io.nowMs/io.parkForIo;通用字节助手io.toBytes/fromBytes亦自net.n移入io包,r1补注5);
DNS解析与ICMP echo经`submitWork`提交VM通用工作线程池跑(无GIL),经
self-pipe回联唤醒。`stdlib/net.n`承载权威声明,公开方法一律脚本方法,脚本层组装
`readLine`/`readExactly`/`write(string)`与带out参数的`recvFrom`包装。

**技术栈:** C++17、Winsock2 + vendored wepoll(IOCP,仅编入nlang_vm)、POSIX骨架、
CMake、VM外native模块通道(NativeHost ABI v11)、e2e(`.n`真实编译执行)+ ctest(C++)。

**Spec:** `docs/dev/net_library_design.md`(r1+r1补注1/2/3/4/5,见文末修订记录;本计划对
spec的细化见"计划级裁定"R1–R14;通用设施归属见spec§六盘点表)。

## 全局约束

- **前置(T1开工前)**:`git merge master`——master领先dev十二个提交(数组/张量统一wave 1、切片语法、native宿主ABI v10、手册同步基线机制bd91907c/5f023053);T14的`tools/manual_sync_check.py`与`SYNC_BASELINE`仅存在于master,先合避免收尾跨线合并([[nlang-merge-array-layout-consumers]]教训)。2026-11-06 `git merge-tree`干跑零冲突(仅known-limitations en/zh两页双方改动,可自动合并);合并后先跑build+ctest门确认绿基线再开T1。
- C++17;全部代码在`nlang::`命名空间;优先STL,不新增外部依赖(vendored wepoll除外,BSD-2-Clause)。
- 注释一律英文;文件头`/*--- ---*/`;source_size_guard:每文件≤500行、每函数≤50行——家族按spec§六拆TU。
- **ABI:`NLANG_HOST_ABI_VERSION` 10→11**(回调表尾追加六项:`nowMs`/`parkForIo`/`moduleState`/`wakeIoWaiters`/`currentContextId`/`submitWork`,契约见任务5);既有四个native模块与VM同构建重编(版本探针)。net不再占intrinsic ID:任务4加的174..220块在任务6删除,下一空闲回到174;`BindingTable.cpp`的net行与`s_HandleClassNames`四个句柄类名同任务删除。
- **模块native(按名派发)不支持out参数**——带out参数的方法(`UdpSocket.recvFrom`/`IcmpSocket.recvFrom`)必须是net.n脚本层包装(用户函数,写回合法;旧intrinsic通道的`VmExecutorOpsCalls.cpp:201`限制只约束已废弃的intrinsic通道)。
- 静态工厂=脚本静态方法(net.n `public static TcpListener bind(...)`,调内部namespace native;无绑定表行、无s_Faces行——计划级裁定R1);同名重载用默认参数归一(如`write(byte[] data, int count = -1)`)。
- Windows:`nlang_net`模块链接`ws2_32`+`iphlpapi`(PRIVATE);`nlang_vm`保留`ws2_32`(wepoll内部用winsock)、去掉`iphlpapi`;wepoll vendored到`src/3rdparty/wepoll/`并登记LICENSE,**仅编入nlang_vm**(模块不poll,只经parkForIo登记兴趣);POSIX骨架v1编译通过即可。
- e2e hermetic:一律绑回环——IPv4用`127.0.0.1`,IPv6用例绑`::`连`[::1]`,组播用例绑`0.0.0.0`(join接口=默认出口接口,回环副本才投递);端口一律0(经`localPort()`读回)、30s内自退出;多载体变体用`.flags`。
- 版本提升至**0.13.0**;`CHANGELOG.md`/`CHANGELOG.zh-CN.md`同步;手册en/zh成对;手册```nlang围栏含`main(`即被docs审计真实编译执行(snippets.py规则),网络片段必须自退出。
- 门禁只认`tools/gate.sh`判决行/退出码;全套含IDE树ctest、Debug断言冒烟、docs strict(见[[nlang-ide-tree-gate-requirement]]、[[nlang-cleanroom-gate-recipe]]记忆)。
- TDD:先写失败测试(e2e或ctest)再实现;真实编译`.n`+真实执行字节码。
- 提交信息英文Conventional Commits;每任务干净退出(测试全绿+零新发现)后提交。
- 构建:`cmake --build build-dev --config Release`(dev工作树的已配置树是`build-dev`,不是`build`);e2e:`D:/dev/miniconda3/envs/py313/python.exe tests/e2e/run_e2e_tests.py E:/cases/nlang/dev/build-dev/src/tools/ncc/Release/ncc.exe E:/cases/nlang/dev/build-dev/src/tools/nvm/Release/nvm.exe`(runner默认`build/`树,必须显式传build-dev二进制绝对正斜杠路径)。

## 审查焦点(Review Focus)

spec未逐一写出、但使用者最可能踩中的输入类/失效模式;每条在归属任务里钉测试:

1. **超时与数据就绪同刻竞态**:唤醒后先重试操作、再查截止——数据已到则返回,不因"已过截止"误报超时。测试:`stdlib_net_tcp_read_timeout.n`(无数据→TimeoutException)+ 同文件迟到数据变体(数据恰在截止前到→成功)。→任务7
2. **reader停泊时另一上下文close**:停泊方得`SocketException("socket closed")`而非挂死。测试:`stdlib_net_tcp_close_wakes_reader.n`。→任务7
3. **write到已关闭对端**:得`SocketException`,不挂死、不静默半写。测试:`stdlib_net_tcp_write_closed.n`。→任务7
4. **停泊期间GC风暴**:停泊帧内byte[]缓冲引用被精确根集覆盖,唤醒后缓冲完好。测试:`stdlib_net_parked_recv_gc.n`(`--gc-stress=4`)。→任务12
5. **1000停泊连接不占载体**:单载体下千并发回环echo仍须完成(停泊让出载体)。测试:`examples/net_conc_bench.n`(单载体入examples_manifest门)+ `stdlib_net_conc_echo.n`(`--carriers=4`,200连接)。→任务12/13
6. **执行器收尾关闭未关socket**:脚本忘close不得泄漏fd——moduleState析构随执行器关闭全部打开槽;第二个执行器拿到全新槽表(句柄值不跨执行器)。测试:`tests/test_vm/test_module_state.cpp`。→任务6

## 计划级裁定(对spec的细化,实施时不再重新讨论)

- **R1 静态工厂路径(r1修订重写)**:net.n公开类提供`public static`脚本方法(如`public static TcpListener bind(string host, int port)`),内部调模块注册的namespace native(`_tcpBind`等);模块无对象字段访问,receiver只是对象句柄(同tensor fullI内部native先例)。旧"声明类静态native成员+绑定表行"路径随intrinsic通道废弃(BindingTable不再有net行);句柄=脚本类私有int字段+public脚本构造器。
- **R2 out参数包装**:`UdpSocket.recvFrom`/`IcmpSocket.recvFrom`为net.n脚本方法(out参数合法):调内部native `_udpRecvFrom`(停泊接收)+`_udpLastRecvAddress`/`_udpLastRecvPort`(读槽内记录来源);C++把来源存为槽内`std::string`,读取时经宿主回调`MintNewString`——不新增GC根。内部native用下划线前缀命名(词法器`Alpha=[_A-Za-z]`合法),不入用户可见面。
- **R3 write/send计数参数**:spec§五`write(byte[] data)`增默认参`int count = -1`(全写整数组;count≥0只写前count字节);UDP的`send`/`sendTo`同样加`int count = -1`默认参(数据报同样只发前count字节)。echo服务器只回发前n字节的便利(无count参时32字节接收缓冲会全带出去);视图实参由偏移感知助手解析(任务4助手规格)。spec§五行文已由spec r1补注3修订(write/send/sendTo加`int count = -1`)。
- **R4 WSA生命周期落点(r1修订重写)**:移入`nlang_net`模块——`NLANG_DEFINE_NATIVE_INIT`路径进程级`std::once_flag`,首次init内注册`std::atexit(WSACleanup)`。VM轮询器(wepoll=IOCP)不需要WSAStartup,nlang_vm保留ws2_32链接(wepoll内部用winsock错误报告)。效果与spec§六"进程级一次、WSACleanup在宿主退出路径执行"一致,模块边界更干净。
- **R5 close的fd生命周期**:按spec§七引用纪律——槽`closed`后不立即`NetClose(fd)`,待停泊者清零(唤醒方复查closed后释放)再真正关fd;无停泊者时close即关。
- **R6 轮询切片**:有新兴趣时`NetPollerWait`超时取`min(最近截止-now, kIoPollSliceMs=10ms)`,封顶新兴趣登记延迟;无兴趣时cv长睡(复刻定时器线程cv模式)。
- **R7 停泊解卷/重入契约(审查第1轮重写)**:`parkForIo`首入登记兴趣+置当前上下文`m_ioParkPending`(不置armed)并返回0(native拿到0必须立即返回);blocking臂在已提交K6重取尾段`FinishBlockingNative`内处置停泊,顺序沿用已提交约束:`BeginExecuting`→重取GIL→`RemoveNativeRoots`→generation失配/StopRequested检查→见pending→清除→调调度器新增方法`FinishIoPark(id)`(GIL已持有、无需重取;取`ParkContextFrames`的判定半段,该函数先`frames.clear()`故不可直接复用——帧已由`ParkFramesForBlockingNative`移入`ctx->frames`,不再移动):`ctx->state==ParkedIo`则置`framesParked=true`;已`Runnable`(唤醒抢在停泊登记前)则直接入队。然后**不**`RestoreStolenFrames`、**不**写回ret(停泊上下文无在飞结果;不经跨DLL异常传播)→返回枚举`Parked`。顺序两条硬约束(审查第2轮修正;已提交`VmExecutorBlockingNative.cpp:166-170`注释明文):`BeginExecuting`必须先于GIL重取——垃圾收集停世界的会合在持GIL下挂条件变量等待,最后到达的收集者需要GIL,先重取即死锁;pending读取必须在generation/StopRequested检查之后——僵尸载体路径上上下文已随运行拆除析构,先读是悬空读。唤醒(`WakeForIo`)**置**`m_ioParkArmed`并按既有K7握手入队,调度器迁移上下文、重执行`OP_CallNative`;重入时parkForIo见armed→清→返回1→native重试syscall。标志协议:pending由首入置、blocking臂消费;armed由唤醒置、重入消费(原文"首入置armed、唤醒清armed"自相矛盾,本轮修正)。
- **R8 moduleState形态**:`moduleState(tag, ctor, dtor)`——宿主维护每执行器`map<tag, pair<ptr, dtor>>`,每tag首调跑ctor,执行器收尾按tag字典序跑dtor(先于模块卸载;原"逆序"在map下无从实施,措辞修正——net现为唯一有状态模块,无实际析构顺序依赖);**前提(审查第2轮明示)**:dtor在全部载体停止后运行——模块dtor取状态mutex并在条件变量上等在飞作业,若仍有载体停在native内持该mutex等待GIL,将与文档锁序(状态mutex→GIL)构成反向死锁;模块状态对象为C++结构体(槽表/last-error/self-pipe/作业表+状态mutex),不用裸bytes形态。
- **R9 作业通道与每执行器状态加锁(审查第1轮重写)**:作业表存每执行器状态(mutex),工作线程池归VM的VmWorkPool(per-调度器单一通用池,DNS/ICMP以两级优先级队列区分,r1补注4);作业记录存`NetExecState*`+jobId(提交时在调用载体上经moduleState解析——`NativeHost`是每次调用的栈上对象,存其指针调用返回即悬空),fn返回后池向ackFd(=self-pipe写端)写1字节(loopback TCP对——wepoll只认socket);停泊上下文唤醒后先清pipe字节再查作业(电平触发防自旋);记录done/released双标志齐才释放。**状态加锁依据**:已提交`NetSocketTable`头注释的加锁前提是"一切调用在VM全局锁下",r1下blocking native释放GIL后跨载体并发——`NetExecState`增`std::mutex`覆盖槽表、`lastErrorText`与等待者纪律:等待方临界区{查closed→置waiter→parkForIo首入登记},关闭方临界区{MarkClosed→读waiter标志},释放锁后再调`wakeIoWaiters(fd)`(关丢唤醒窗:先close→等待方见closed;先置waiter→关闭方见置位);锁序状态mutex→GIL(parkForIo/wakeIoWaiters内部取GIL时持状态锁,轮询器只GIL下触调度器态,无反序)。执行器析构:dtor先置shutdown并等在飞作业到done(`MarkDone`通知cv)再释放状态——worker不写已释放内存。
- **R10 错误码表**:native返回负码-1 closed/-2 timeout/-3并发违例/-4平台错误/-5非法参数;脚本`_throwNet`映射TimeoutException/SocketException(-4拼`_lastErrorText()`)。
- **R11 句柄与命名**:句柄=脚本类私有int字段+public脚本构造器;内部native下划线前缀命名合法;异常子类两参`super(message, cause)`委托有e2e先例(super_with_args.n),退路=super(message)+cause字段拼消息(任务6探针)。
- **R12 截止传递**:内部native收绝对截止(long,io.nowMs同域,≤0=无截止);公开方法把timeoutMs换算一次,绝对截止跨重入/跨多段操作稳定(readLine/readExactly共享同一截止)。
- **R13 wakeIoWaiters**:close native(blocking通道、无GIL)需注销兴趣+唤醒停泊等待者——ABI v11第四回调`wakeIoWaiters(fd)`(宿主取GIL→从`m_ioWaits[fd]`收集等待者id并注销兴趣→逐个`WakeForIo`;已提交`RemoveIoWaitsForFd`只注销不唤醒,唤醒语义由本回调补);并入任务5。
- **R14 currentContextId(审查第1轮新增)**:模块的每方向waiter守卫须区分"本调用重入"与"他人并发抢占"(已提交槽表waiter字段=上下文id,0=无;`ReleaseWaiter`无条件清向,误清他人waiter会破坏R5延迟关fd判定),而模块在blocking通道无上下文身份、ABI v11前四项不携带——增第五回调`currentContextId(self)`(宿主返调度器当前上下文id,跨停泊/重入稳定)。VM保持通用:拒绝策略留在模块,`parkForIo`不掺net语义。

## 文件结构

```
已提交(任务1-4历史形态;任务6按r1架构搬迁/解耦):
  src/3rdparty/wepoll/{wepoll.c,wepoll.h,LICENSE.txt}            vendored(IOCP epoll兼容,BSD-2-Clause)
  src/vm/net/NetPlatform.h / NetPlatformInternal.h / NetPlatformWindows.cpp / NetPlatformWindowsIcmp.cpp / NetPlatformPosix.cpp  平台抽象(syscall层)
  src/vm/net/NetSocketTable.h/.cpp                              每执行器socket槽表(代际防ABA)
  src/vm/VmSchedulerIo.cpp                                      兴趣表/轮询线程/ParkForIo
  src/vm/IntrinsicsNetSupport.cpp / IntrinsicsNetTcp.cpp / IntrinsicsNetUdp.cpp / IntrinsicsNetIcmp.cpp  intrinsic家族(任务6删除)
  include/nlang/vm/Intrinsics.h ID 174..220块、BindingTable.cpp net行、stdlib/net.n初版、
  tests/test_vm/test_net_platform.cpp / test_net_slots.cpp / test_net_scheduler.cpp、
  tests/e2e/stdlib_net_bytes_roundtrip.n / stdlib_net_tcp_bind_probe.n / array_size_call_expr.n
新建  src/vm/IoPoller.h / IoPollerWindows.cpp / IoPollerPosix.cpp  通用fd readiness抽象(从NetPlatform拆出,wepoll/epoll留VM)
新建  src/vm/VmWorkPool.h / VmWorkPool.cpp  通用原生工作线程池(并发基础设施,per-调度器惰性起停、与轮询线程同模板):submitWork(fn,arg,ackFd,priority),worker无GIL跑fn、完成向ackFd写一字节(r1补注4)
搬迁  src/vm/net/NetPlatform.h→net_platform.h、NetPlatformInternal.h→net_platform_internal.h、NetPlatformWindows.cpp→net_platform_windows.cpp、NetPlatformWindowsIcmp.cpp→net_platform_icmp_windows.cpp、NetPlatformPosix.cpp→net_platform_posix.cpp、NetSocketTable.{h,cpp}→net_slots.{h,cpp}  syscall层与槽表git mv入模块
新建  src/native/net/net_native.cpp net_state.{h,cpp} net_tcp.cpp net_udp.cpp net_icmp.cpp net_jobs.{h,cpp}  模块TU(入口/每执行器状态/TCP/UDP/ICMP/作业表+自管道ack;工作线程池归VM,经submitWork提交)
修改  include/nlang/vm/NativeHost.h                             ABI v11六回调
修改  include/nlang/vm/Intrinsics.h                             删174..220块(下一空闲回174)
修改  src/compiler/builder/BindingTable.cpp                     删net行+s_HandleClassNames四条
修改  src/vm/VmExecutorIntrinsics.cpp                           删net级联
修改  src/vm/VmExecutor.h / VmExecutor.cpp                      删net成员/idx缓存/收尾关fd
修改  src/vm/VmExecutorNativeHost.cpp                           v11接线+六实现
修改  src/vm/VmExecutorBlockingNative.cpp                       park-pending处置(R7)
修改  src/vm/VmScheduler.h                                      armed/pending标志、WakeForIo置armed、FinishIoPark(R7)
修改  src/vm/VmSchedulerIo.cpp                                  include改IoPoller.h
修改  src/vm/CMakeLists.txt                                     删net TU/IntrinsicsNetSupport/iphlpapi(wepoll留)
修改  src/native/CMakeLists.txt                                 注册nlang_net(target_sources+ws2_32/iphlpapi+install)
修改  stdlib/io.n                                               加nowMs/parkForIo/toBytes/fromBytes(字节助手自net.n移入,r1补注5)
修改  stdlib/net.n                                              全文重写(纯脚本异常/脚本类/内部native;公开toBytes/fromBytes删除)
改名  tests/e2e/stdlib_net_bytes_roundtrip.n→stdlib_io_bytes_roundtrip.n  字节往返用例随助手迁入io包(r1补注5)
新建  tests/test_vm/test_work_pool.cpp                          VmWorkPool单测(提交→ack字节、两级优先级、惰性起停)
新建  tests/test_vm/test_module_state.cpp                       执行器隔离+析构关fd(审查焦点6)
修改  tests/CMakeLists.txt                                      test_net_platform/test_net_slots改编模块源(不链nlang_vm)+注册test_module_state
修改  tests/test_vm/test_net_platform.cpp / test_net_slots.cpp / test_net_scheduler.cpp  poller用例并入调度器测试
修改  tests/test_compiler/test_library_index.cpp                177→179→187→199→203→205→223→229(逐任务)
修改  tests/test_langservice/test_symbol_index.cpp              同计数
新建  tests/e2e/stdlib_io_park_now.n                            io停泊e2e
新建  tests/e2e/stdlib_net_*.n(+.flags)                         17个新net e2e
修改  tests/e2e/manifest.txt                                    +18行
新建  examples/net_echo_server.n net_echo_client.n net_http_mini.n net_udp_echo.n net_ping.n net_conc_bench.n
修改  tests/e2e/examples_manifest.txt                           6行
新建  docs/user_manual/{en,zh}/specification/library/20-network/{index,tcp,udp,icmp}.md
修改  docs/user_manual/{en,zh}/specification/library/11-conventions.md 19-library-exceptions.md index.md
修改  docs/user_manual/{en,zh}/getting-started/install-layout.md mkdocs.en.yml mkdocs.zh.yml
修改  CHANGELOG.md CHANGELOG.zh-CN.md VERSION docs/user_manual/SYNC_BASELINE
新建  tools/net_bench/net_bench_10k.n                           本地基准(不入ctest;ab/wrk吞吐口径)
```

## 任务依赖

```
T1平台层 → T2槽表 → T3调度器停泊 → T4包骨架(任务1-4已提交,历史形态)
T5 ABI v11+io停泊面 → T6 模块搬迁/解耦+net.n重写 → T7 TCP核心 → T8 TCP选项 → T9 脚本分帧
T7 → T10 UDP → T11 ICMP+ping → T12 并发/GC加固 → T13 示例 → T14 文档 → T15 全套门禁+10k基准
```
任务5-15全文见本文件后半(任务1-4文本为已提交历史,保留原样;其文件路径以任务6搬迁后的新布局为准)。

---

### 任务1:平台抽象层 + wepoll vendoring + CMake接线

**文件:**
- 新建:`src/3rdparty/wepoll/wepoll.c`、`wepoll.h`、`LICENSE.txt`
- 新建:`src/vm/net/NetPlatform.h`、`src/vm/net/NetPlatformWindows.cpp`、`src/vm/net/NetPlatformPosix.cpp`
- 修改:`src/vm/CMakeLists.txt`
- 测试:`tests/test_vm/test_net_platform.cpp`、`tests/CMakeLists.txt`

**接口:**
- 产出:`nlang::`自由函数`NetPlatformInit/NetOpenTcpStream/NetOpenUdp/NetOpenIcmp/NetBind/NetListen/NetConnect/NetAccept/NetRecv/NetSend/NetRecvFrom/NetSendTo/NetShutdownWrite/NetClose/NetConnectResult/NetLocalPort/NetLocalAddress/NetPeerPort/NetPeerAddress/NetSetNoDelay/NetSetKeepAlive/NetSetSendBuffer/NetSetRecvBuffer/NetSetBroadcast/NetJoinMulticast/NetLeaveMulticast/NetSetMulticastTtl/NetSetTtl/NetPollerCreate/NetPollerClose/NetPollerAdd/NetPollerMod/NetPollerDel/NetPollerWait/NetResolve/NetIcmpEcho/NetIcmpSend/NetIcmpRecv/NetLastErrorText`(任务3/5/8/9消费)。

- [ ] **步骤1:vendor wepoll**。从https://github.com/piscisaureus/wepoll 的dist分支取`wepoll.c`/`wepoll.h`(BSD-2-Clause,Bert Belder 2012-2020),连同BSD-2-Clause全文存为`LICENSE.txt`。导出`epoll_create1/epoll_ctl/epoll_wait/epoll_close`(`WEPOLL_EXPORT`默认为空→静态编入nlang_vm.dll不导出),不与ws2_32符号冲突。(本行按已提交态更正——原文piscator/MIT/`we_`前缀系误述,见任务1台账裁决。)

- [ ] **步骤2:写失败测试**`tests/test_vm/test_net_platform.cpp`(纯平台层,无VM;仿`test_domainlocks`的CHECK宏风格):

```cpp
/*---
    test_net_platform.cpp — the OS socket abstraction contract: loopback
    TCP/UDP round trips, the poller set, numeric resolution, and the
    unprivileged ICMP echo. No VM, no scheduler.
---*/
#include "net/NetPlatform.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static void TestTcpLoopbackRoundTrip() {
    CHECK(NetPlatformInit());
    int ls = NetOpenTcpStream(NetFamily::Inet);
    CHECK(ls >= 0);
    CHECK(NetBind(ls, "127.0.0.1", 0) == 0);
    CHECK(NetListen(ls, 8) == 0);
    const int port = NetLocalPort(ls);
    CHECK(port > 0);
    //Client connects; accept on the listener must report readable.
    int cli = NetOpenTcpStream(NetFamily::Inet);
    CHECK(NetConnect(cli, "127.0.0.1", port) == 0);
    int srv = -1;
    for (int spin = 0; spin < 1000 && (srv = NetAccept(ls)) < 0; ++spin)
        ;   //loopback connect is immediate; bounded spin, no sleep
    CHECK(srv >= 0);
    const uint8_t msg[3] = { 10, 20, 30 };
    CHECK(NetSend(cli, msg, 3) == 3);
    uint8_t got[3] = { 0, 0, 0 };
    int n = -1;
    for (int spin = 0; spin < 1000 && ((n = NetRecv(srv, got, 3)) < 0); ++spin)
        ;
    CHECK(n == 3);
    CHECK(got[0] == 10 && got[1] == 20 && got[2] == 30);
    //EOF after peer shutdown.
    NetShutdownWrite(cli);
    NetClose(cli);
    n = -1;
    for (int spin = 0; spin < 10000 && ((n = NetRecv(srv, got, 3)) > 0); ++spin)
        ;
    CHECK(n == 0);   //EOF
    NetClose(srv);
    NetClose(ls);
}

static void TestWouldBlock() {
    int ls = NetOpenTcpStream(NetFamily::Inet);
    CHECK(NetBind(ls, "127.0.0.1", 0) == 0);
    CHECK(NetListen(ls, 1) == 0);
    CHECK(NetAccept(ls) == kNetWouldBlock);   //empty backlog
    NetClose(ls);
}

static void TestUdpRoundTrip() {
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    CHECK(NetBind(a, "127.0.0.1", 0) == 0);
    CHECK(NetBind(b, "127.0.0.1", 0) == 0);
    const uint8_t msg[2] = { 7, 8 };
    CHECK(NetSendTo(a, msg, 2, "127.0.0.1", NetLocalPort(b)) == 2);
    uint8_t got[4] = { 0, 0, 0, 0 };
    std::string from;
    int fromPort = 0;
    int n = -1;
    for (int spin = 0; spin < 10000 && ((n = NetRecvFrom(b, got, 4, from, fromPort)) < 0); ++spin)
        ;
    CHECK(n == 2);
    CHECK(from == "127.0.0.1");
    CHECK(fromPort == NetLocalPort(a));
    NetClose(a);
    NetClose(b);
}

static void TestPoller() {
    void* p = NetPollerCreate();
    CHECK(p != nullptr);
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    CHECK(NetPollerAdd(p, b, NET_EV_READ) == 0);
    std::vector<NetPollEvent> evs;
    CHECK(NetPollerWait(p, evs, 10) == 0);        //no traffic: empty
    CHECK(evs.empty());
    const uint8_t m[1] = { 1 };
    NetSendTo(a, m, 1, "127.0.0.1", NetLocalPort(b));
    CHECK(NetPollerWait(p, evs, 2000) > 0);
    CHECK(!evs.empty() && evs[0].fd == b && (evs[0].events & NET_EV_READ));
    NetPollerDel(p, b);
    NetClose(a);
    NetClose(b);
    NetPollerClose(p);
}

static void TestResolveAndIcmp() {
    std::vector<NetEndpoint> eps;
    CHECK(NetResolve("localhost", 80, eps));
    CHECK(!eps.empty());
    //Unprivileged echo (IcmpSendEcho2 / DGRAM ICMP): loopback answers.
    const uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    const int rtt = NetIcmpEcho("127.0.0.1", payload, 4, 3000);
    CHECK(rtt >= 0);   //-1 timeout, -2 error — loopback must answer
}

int main() {
    TestTcpLoopbackRoundTrip();
    TestWouldBlock();
    TestUdpRoundTrip();
    TestPoller();
    TestResolveAndIcmp();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤3:注册ctest**(`tests/CMakeLists.txt`,紧跟`add_test(NAME domainlocks_tests ...)`之后):

```cmake
# v0.13.0 net: the OS socket abstraction contract (loopback TCP/UDP,
# poller set, numeric resolution, unprivileged ICMP echo) — no VM.
add_executable(test_net_platform
    test_vm/test_net_platform.cpp
)
target_include_directories(test_net_platform
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_net_platform
    PRIVATE
        nlang_vm
)

add_test(NAME net_platform_tests COMMAND test_net_platform)
```

- [ ] **步骤4:跑测试确认编译失败**(缺头文件)。
  `cmake --build build-dev --config Release --target test_net_platform`

- [ ] **步骤5:写`src/vm/net/NetPlatform.h`**(完整):

```cpp
/*---
    NetPlatform.h — the OS socket abstraction of the net package
    (net library design §6). One header, two implementations
    (NetPlatformWindows.cpp / NetPlatformPosix.cpp). All sockets are
    non-blocking; "no data / no space" surfaces as kNetWouldBlock, an
    in-flight connect as kNetInProgress. Blocking conveniences
    (NetResolve, NetIcmpEcho) run only on the dedicated resolver /
    icmp worker threads — never on a carrier.

    Error convention: -1 = failure with NetLastErrorText() carrying a
    human-readable OS message; kNetWouldBlock / kNetInProgress are
    non-failure status codes. A fd is a plain OS socket handle.
---*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//NetPollerWait event bits (a subset of the wepoll/epoll vocabulary).
constexpr uint32_t NET_EV_READ = 1u;
constexpr uint32_t NET_EV_WRITE = 2u;
constexpr uint32_t NET_EV_HUP = 4u;
constexpr uint32_t NET_EV_ERR = 8u;

//Non-failure status codes returned where -1 means "failed".
constexpr int kNetWouldBlock = -1000;
constexpr int kNetInProgress = -1001;

//Address family. The neutral header keeps its own enum: AF_INET6 is
//23 on Windows but 10 on Linux/10 on macOS, so callers must not pass
//raw AF_* values; each platform TU maps this enum to its AF_*.
enum class NetFamily { Inet, Inet6 };

//One resolved endpoint: numeric address text + port + family
//(spec §三: IPv6 literals and dual-stack listen ship in v1; sockets
//open with the endpoint's family).
struct NetEndpoint {
    std::string address;
    int port = 0;
    NetFamily family = NetFamily::Inet;
};

//One poller completion.
struct NetPollEvent {
    int fd = 0;
    uint32_t events = 0;
};

//Process-level socket subsystem init (WSAStartup on Windows).
//Idempotent through a once flag (plan ruling R4); every net entry
//point calls it first. First init registers atexit(WSACleanup) for
//the host exit path (spec §6).
bool NetPlatformInit();

//--- TCP -----------------------------------------------------------------
int NetOpenTcpStream(NetFamily family);
//port 0 = auto; addr "::" → dual-stack listener (IPV6_V6ONLY=0 set
//before bind, IPv4 peers arrive v4-mapped).
int NetBind(int fd, const std::string& addr, int port);
int NetListen(int fd, int backlog);
//Non-blocking: 0 = connected now, kNetInProgress = in flight (judge
//later with NetConnectResult), kNetWouldBlock = backlog empty,
//-1 = failed.
int NetConnect(int fd, const std::string& addr, int port);
int NetAccept(int fd);
//0 = peer closed (EOF). Stream send returns bytes written (may be <
//len; the caller loops).
int NetRecv(int fd, uint8_t* dst, int len);
int NetSend(int fd, const uint8_t* src, int len);
int NetShutdownWrite(int fd);
//Post-kNetInProgress verdict: 0 = ok, -1 = failed (reason in
//NetLastErrorText).
int NetConnectResult(int fd);

//--- UDP -----------------------------------------------------------------
int NetOpenUdp(NetFamily family);
int NetRecvFrom(int fd, uint8_t* dst, int len,
                std::string& outAddress, int& outPort);
int NetSendTo(int fd, const uint8_t* src, int len,
              const std::string& addr, int port);

//--- shared socket state --------------------------------------------------
void NetClose(int fd);
int NetLocalPort(int fd);
std::string NetLocalAddress(int fd);
int NetPeerPort(int fd);
//v4-mapped IPv6 endpoints print as plain dotted IPv4
//(IN6_IS_ADDR_V4MAPPED → unwrap), so dual-stack listeners report
//IPv4 peers the way the script layer bound them.
std::string NetPeerAddress(int fd);
bool NetSetNoDelay(int fd, bool on);
bool NetSetKeepAlive(int fd, bool on);
bool NetSetSendBuffer(int fd, int bytes);
bool NetSetRecvBuffer(int fd, int bytes);
bool NetSetBroadcast(int fd, bool on);
//imr_interface = the socket's bound address (INADDR_ANY when
//wildcard/unbound) so a self-join + loopback send delivers the loop
//copy on the same interface the send leaves by. IPv6 groups use
//IPV6_JOIN_GROUP with the bound v6 address (or 0).
bool NetJoinMulticast(int fd, const std::string& group);
bool NetLeaveMulticast(int fd, const std::string& group);
bool NetSetMulticastTtl(int fd, int ttl);
bool NetSetTtl(int fd, int ttl);

//--- poller set (wepoll on Windows, epoll on POSIX) ------------------------
void* NetPollerCreate();
void NetPollerClose(void* poller);
int NetPollerAdd(void* poller, int fd, uint32_t events);
int NetPollerMod(void* poller, int fd, uint32_t events);
int NetPollerDel(void* poller, int fd);
//Fills out with ready events, returns their count (0 on timeout).
int NetPollerWait(void* poller, std::vector<NetPollEvent>& out, int timeoutMs);

//--- blocking conveniences (worker threads only) ---------------------------
//Numeric-literal fast path (inet_pton AF_INET then AF_INET6,
//stripping surrounding brackets — "[::1]" form, spec D10) over
//getaddrinfo(AF_UNSPEC); fills endpoints of both families in resolver
//order. connect tries them in order (v6-first stacks still fall back
//to v4 when the listener is v4-only).
bool NetResolve(const std::string& host, int port,
                std::vector<NetEndpoint>& out);
//ICMP echo round trip: returns RTT milliseconds, -1 = timeout,
//-2 = unreachable/error. Resolves first; prefers the first IPv4
//endpoint (IcmpSendEcho2, unprivileged), else the first IPv6
//(Icmp6SendEcho2 with source "::"). POSIX skeleton: DGRAM ICMP.
int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len,
                int timeoutMs);

//--- raw ICMP sockets (IcmpSocket) -----------------------------------------
//Windows: SOCK_RAW/IPPROTO_ICMP (admin; bind reports the admin
//requirement through NetLastErrorText). POSIX: SOCK_DGRAM/IPPROTO_ICMP.
//v1 scope: IPv4 raw only (AF_INET); IPv6 echo goes through
//NetIcmpEcho (ping), not through IcmpSocket.
int NetOpenIcmp();
int NetIcmpSend(int fd, const uint8_t* msg, int len,
                const std::string& addr);
int NetIcmpRecv(int fd, uint8_t* dst, int len, std::string& outAddress);

//Human-readable last OS error (WSAGetLastError / errno mapped).
std::string NetLastErrorText();

}  //namespace nlang
```

- [ ] **步骤6:写`NetPlatformWindows.cpp`**。winsock2 + iphlpapi(icmpapi)+ wepoll映射:
  - 所有open后`ioctlsocket(FIONBIO,1)`;`NetPollerCreate`=`we_poll_create(64)`;`NetPollerWait`=`we_poll_wait(p,epd,n,timeoutMs)`,`EPOLLIN→NET_EV_READ`等映射;`NetResolve`先`inet_pton(AF_INET/AF_INET6)`快径(剥`[...]`括号)再`getaddrinfo(AF_UNSPEC)`(两族endpoint按解析序);`NetIcmpEcho`用`IcmpCreateFile`+`IcmpSendEcho2`(v4)/`Icmp6SendEcho2`(v6,源`::`;同步,超时参数=timeoutMs,应答`status==0`→RTT取`Reply->RoundTripTime`);`NetOpenIcmp`=`socket(AF_INET,SOCK_RAW,IPPROTO_ICMP)`,`NetBind`对`WSAEACCES/WSAEADDRNOTAVAIL`置文本"raw ICMP requires administrator privileges on Windows";`NetIcmpSend/Recv`走`sendto/recvfrom`。每函数≤50行,公共小助手`SetSockOptInt(fd,level,name,value)`。
- [ ] **步骤7:写`NetPlatformPosix.cpp`**骨架:`#include <sys/socket.h>...` + epoll实现`NetPoller*`(epoll_create1/epoll_ctl/epoll_wait真实现,移植成本为零),`NetIcmpEcho`用DGRAM ICMP sendto+recvfrom带`SO_RCVTIMEO`,`NetOpenIcmp`=SOCK_DGRAM/IPPROTO_ICMP。Windows外仅此文件参与编译。
- [ ] **步骤8:CMake接线**`src/vm/CMakeLists.txt`:

```cmake
add_library(nlang_vm
    ...
    NetResolver.cpp            #任务5加入,此处先不加
    VmSchedulerIo.cpp          #任务3加入
    net/NetSocketTable.cpp     #任务2加入
    net/NetPlatformWindows.cpp
    net/NetPlatformPosix.cpp
    ...
)
```
实际本任务只加`net/NetPlatformWindows.cpp`与`net/NetPlatformPosix.cpp`两行(其余任务各自加),并在文件尾加:

```cmake
if (WIN32)
    #v0.13.0 net: vendored wepoll (IOCP-backed epoll-compatible poller,
    #BSD-2-Clause — src/3rdparty/wepoll/LICENSE.txt) + winsock2 + iphlpapi
    #(IcmpSendEcho2). The POSIX branch builds NetPlatformPosix.cpp.
    target_sources(nlang_vm PRIVATE
        ${PROJECT_SOURCE_DIR}/src/3rdparty/wepoll/wepoll.c)
    target_include_directories(nlang_vm PRIVATE
        ${PROJECT_SOURCE_DIR}/src/3rdparty/wepoll)
    target_link_libraries(nlang_vm PRIVATE ws2_32 iphlpapi)
    #wepoll is third-party C: keep it out of our /W4 warnings-as-errors
    #bar (tinyxml2 precedent: third-party code keeps its own bar).
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/src/3rdparty/wepoll/wepoll.c
        PROPERTIES COMPILE_OPTIONS "/W3")
endif()
```
非WIN32分支:`target_sources(nlang_vm PRIVATE net/NetPlatformPosix.cpp)`由add_library源列表按平台条件挑选——实现方式:add_library列表里两文件都列,再用`set_source_files_properties(... STALELY/GENERATED)`不稳;直接做法=按平台`if(WIN32)`只列对应文件(本步采用:两行各包进if/else)。

- [ ] **步骤9:构建并跑绿**:`cmake --build build-dev --config Release --target test_net_platform && build-dev/tests/Release/test_net_platform.exe`(或`ctest --test-dir build-dev -C Release -R net_platform`)。
- [ ] **步骤10:source_size_guard自检**后提交:

```bash
git add src/3rdparty/wepoll src/vm/net src/vm/CMakeLists.txt tests/test_vm/test_net_platform.cpp tests/CMakeLists.txt
git commit -m "feat(net): platform socket abstraction with vendored wepoll"
```

**验收判据:**`net_platform_tests`绿;`GATE ... PASS`。

---

### 任务2:socket槽表NetSocketTable

**文件:**
- 新建:`src/vm/net/NetSocketTable.h`、`src/vm/net/NetSocketTable.cpp`
- 测试:`tests/test_vm/test_net_slots.cpp`、`tests/CMakeLists.txt`

**接口:**
- 消费:任务1的`NetClose`。
- 产出:`NetSocketSlot`字段与`NetSocketTable::{Mint,Lookup,Alive,MarkClosed,ReleaseWaiter,CloseIfDrained,OpenCount}`(任务3/5/8/9消费;VmExecutor持有一个实例)。

- [ ] **步骤1:写失败测试**`tests/test_vm/test_net_slots.cpp`:

```cpp
/*---
    test_net_slots.cpp — the per-executor socket slot table: handle
    minting with a generation tag (ABA), closed/lookup rules, and the
    close-waits-for-waiters fd lifetime rule (plan ruling R5).
---*/
#include "net/NetSocketTable.h"
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static void TestMintLookupGeneration() {
    NetSocketTable table;
    const int32_t h = table.Mint(/*fd*/5, kNetKindTcpStream);
    CHECK(h != 0);
    NetSocketSlot* s = table.Lookup(h);
    CHECK(s != nullptr && s->fd == 5 && !s->closed);
    table.MarkClosed(h);
    CHECK(table.Lookup(h) == nullptr);          //closed slots reject
    //Free + remint: the generation differs, the stale handle rejects.
    table.ReleaseFree(h);
    const int32_t h2 = table.Mint(/*fd*/9, kNetKindTcpStream);
    CHECK(table.Lookup(h) == nullptr);          //stale generation
    CHECK(table.Lookup(h2) != nullptr);
}

static void TestCloseWaitsForWaiters() {
    NetSocketTable table;
    const int32_t h = table.Mint(7, kNetKindUdp);
    table.SetWaiter(h, NetDir::Read, /*ctxId*/42);
    table.MarkClosed(h);
    CHECK(table.FdOf(h) == 7);                  //fd kept while parked
    table.ReleaseWaiter(h, NetDir::Read);
    CHECK(table.FdOf(h) == -1);                 //last waiter drained:
                                                //the real close ran
    CHECK(table.OpenCount() == 0);
}

int main() {
    TestMintLookupGeneration();
    TestCloseWaitsForWaiters();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤2:注册ctest**(同任务1模式,名`net_slots_tests`,源`test_vm/test_net_slots.cpp`)。
- [ ] **步骤3:实现`NetSocketTable.h`**(完整):

```cpp
/*---
    NetSocketTable.h — the per-VmExecutor socket slot registry (net
    library design §6): __handle values are (generation<<kHandleIndexBits
    | (index+1)) so a closed slot's recycled index never aliases a live
    handle (ABA). The fd close is deferred while waiters are parked on
    the slot (plan ruling R5): MarkClosed only flags, the last
    ReleaseWaiter performs the real NetClose.

    Locking: every call runs under the VM global lock (the intrinsics
    that touch the table hold the GIL; the poller thread takes the GIL
    before harvesting) — the table itself takes no lock.
---*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Slot kinds (the __handle carries no kind; the slot does).
constexpr uint8_t kNetKindTcpListener = 1;
constexpr uint8_t kNetKindTcpStream = 2;
constexpr uint8_t kNetKindUdp = 3;
constexpr uint8_t kNetKindIcmp = 4;

//Waiter directions (stored on the slot, not in ContextState — one
//ParkedIo state serves every direction).
enum class NetDir : uint8_t { Read, Write, Accept };

struct NetSocketSlot {
    int fd = -1;
    uint8_t kind = 0;            //0 = free
    bool closed = false;
    int generation = 0;          //increments on every free
    int acceptWaiter = 0;        //context id, 0 = none
    int readWaiter = 0;
    int writeWaiter = 0;
    uint32_t interest = 0;       //NET_EV_* currently registered
    //Last recvFrom source (UDP/ICMP): plain text, minted to a string
    //on demand — no GC root.
    std::string lastPeerAddress;
    int lastPeerPort = 0;
};

class NetSocketTable {
public:
    //Takes the fd into a free slot (appends when none free).
    int32_t Mint(int fd, uint8_t kind);
    //The slot for a handle, or null when the handle is stale (index
    //freed/reused) or the slot is closed.
    NetSocketSlot* Lookup(int32_t handle);
    const NetSocketSlot* Lookup(int32_t handle) const;
    void SetWaiter(int32_t handle, NetDir dir, int contextId);
    //Clears the direction's waiter id; when the slot is closed and no
    //waiter remains, performs the deferred real fd close.
    void ReleaseWaiter(int32_t handle, NetDir dir);
    //Flags closed. With no waiters the fd closes immediately;
    //otherwise the last ReleaseWaiter closes it (R5).
    void MarkClosed(int32_t handle);
    //Returns the slot's fd (-1 once really closed). Used by close/
    //option arms before the closed check.
    int FdOf(int32_t handle) const;
    //Returns the slot to the free list (generation++). Precondition:
    //closed and no waiters (asserted).
    void ReleaseFree(int32_t handle);
    int OpenCount() const;   //slots with a live fd (leak assertions)

private:
    static constexpr int32_t kHandleIndexBits = 24;
    std::vector<NetSocketSlot> m_slots;
    std::vector<int32_t> m_freeList;
};

}  //namespace nlang
```

- [ ] **步骤4:实现`NetSocketTable.cpp`**(每函数≤20行;`MarkClosed`里`if (noWaiter) { NetClose(fd); fd=-1; }`否则留待`ReleaseWaiter`;`ReleaseFree`断言closed且无waiter)。`src/vm/CMakeLists.txt`加`net/NetSocketTable.cpp`。
- [ ] **步骤5:跑绿**`ctest --test-dir build-dev -C Release -R net_slots`(经gate.sh)。
- [ ] **步骤6:提交**`git commit -m "feat(net): per-executor socket slot table with generation handles"`。

---

### 任务3:调度器ParkForIo + IO轮询线程

**文件:**
- 新建:`src/vm/VmSchedulerIo.cpp`
- 修改:`src/vm/VmScheduler.h`(Reason::Io、ContextState::ParkedIo、IoWaitEntry、成员、声明)、`src/vm/VmScheduler.cpp`(析构join)、`src/vm/CMakeLists.txt`
- 测试:`tests/test_vm/test_net_scheduler.cpp`、`tests/CMakeLists.txt`

**接口:**
- 消费:任务1轮询接口、任务2槽表(仅测试里配合)。
- 产出:`VmScheduler::ParkForIo(int id, int fd, uint32_t events, std::chrono::steady_clock::time_point deadline, bool hasDeadline)`、`WakeForIo(int id)`、`RemoveIoWaitsForFd(int fd)`(任务5/8/9消费)。

- [ ] **步骤1:写失败测试**`tests/test_vm/test_net_scheduler.cpp`——进程内调度器级(仿`test_gcworldstop`直接操作调度器;先`Runtime::StaticInit()`,记忆[[nlang-inprocess-test-host-init]]):

```cpp
/*---
    test_net_scheduler.cpp — the IO parking contract: a parked context
    wakes on fd readiness, on deadline expiry, and on an explicit
    close-side wake; every wake keeps the framesParked handshake
    (the woken context is enqueued exactly once).
---*/
#include "VmScheduler.h"
#include "net/NetPlatform.h"
#include <nlang/runtime/Runtime.h>
#include <chrono>
#include <cstdio>
#include <thread>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

using namespace nlang;
using namespace std::chrono;

//Drives the scheduler single-threaded like a carrier: register a
//context, park it, then poll the context state for the wake. (Do
//NOT poll PopNextRunnable here: it returns an int context id and
//BLOCKS on an empty queue with deadlock detection — VmScheduler.h
//300/294-305. The test context never parked real frames, so
//WakeForIo only flips state to Runnable without enqueuing.)
static bool WaitUntilRunnable(VmScheduler& sched, int id, int spins) {
    for (int spin = 0; spin < spins; ++spin) {
        if (sched.Find(id)->state == ContextState::Runnable)
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

static void TestWakeOnReadiness() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    sched.ParkForIo(id, b, NET_EV_READ, steady_clock::now() + 5s, true);
    CHECK(sched.Find(id)->state == ContextState::ParkedIo);
    //Traffic from another OS thread: the poller must wake the context.
    std::thread peer([&] {
        const uint8_t m[1] = { 1 };
        std::this_thread::sleep_for(50ms);   //park registered
        NetSendTo(a, m, 1, "127.0.0.1", NetLocalPort(b));
    });
    const bool woke = WaitUntilRunnable(sched, id, 2000);
    peer.join();
    CHECK(woke);
    NetClose(a);
    NetClose(b);
}

static void TestWakeOnDeadline() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, NET_EV_READ, steady_clock::now() + 100ms, true);
    const auto t0 = steady_clock::now();
    CHECK(WaitUntilRunnable(sched, id, 4000));
    CHECK(steady_clock::now() - t0 >= 90ms);   //not an early wake
    NetClose(fd);
}

static void TestRemoveOnClose() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, NET_EV_READ, steady_clock::now() + 30s, true);
    sched.RemoveIoWaitsForFd(fd);   //the close path
    sched.WakeForIo(id);            //the close wakes its waiters
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

int main() {
    Runtime::StaticInit();
    TestWakeOnReadiness();
    TestWakeOnDeadline();
    TestRemoveOnClose();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤2:注册ctest**`net_scheduler_tests`(同前模式)。
- [ ] **步骤3:`VmScheduler.h`增改**(英文注释,与既有中文注释并存):
  - `ParkSignal::Reason`追加`Io`:`enum class Reason { Preempted, Timer, Join, Mutex, ChanRecv, ChanSend, Semaphore, ClassInit, Io };`
  - `ContextState`追加`ParkedIo`(注释:net IO parking — one state for every direction, the direction lives on the socket slot)。
  - public区(`ParkForChanSend`声明之后)加:

```cpp
    //v0.13.0 net: IO parking (net library design §6/§7). The context
    //parks until the poller reports fd+events ready, the deadline
    //passes, or the close path wakes it (WakeForIo / RemoveIoWaitsForFd
    //then WakeForIo). fd < 0 = pure deadline park (resolver/icmp
    //jobs). Mirrors ParkForTimer's lazy thread start and the K7
    //framesParked handshake.
    void ParkForIo(int id, int fd, uint32_t events,
                   std::chrono::steady_clock::time_point deadline,
                   bool hasDeadline);
    //Moves a ParkedIo context back to Runnable (enqueued under the
    //framesParked handshake, WakeForMutex shape).
    void WakeForIo(int id);
    //Drops every interest entry for a closing fd (the poller
    //re-registers interest from the table each loop, so removal here
    //is enough — no wepoll del needed).
    void RemoveIoWaitsForFd(int fd);
```
  - private区加:

```cpp
    //v0.13.0 net: one IO interest entry (the poller's working set).
    struct IoWaitEntry {
        int contextId = 0;
        int fd = -1;             //-1 = deadline-only (no poll interest)
        uint32_t events = 0;
        std::chrono::steady_clock::time_point deadline;
        bool hasDeadline = false;
    };
    void IoPollerThreadMain();
    void SyncPollerInterest(void* poller);   //add/mod to match m_ioWaits
    int NextIoTimeoutMs() const;             //nearest deadline, capped
    void HarvestIoEvents(void* poller, std::vector<NetPollEvent>& evs);
                                            //ready + expired wakes
    std::vector<IoWaitEntry> m_ioWaits;      //linear table (timer shape)
    bool m_ioThreadStarted = false;
    bool m_ioThreadShutdown = false;
    std::thread m_ioThread;
    std::condition_variable_any m_ioCv;      //interest/shutdown signal
```
- [ ] **步骤4:`VmScheduler.cpp`析构**:在既有timer join(`VmScheduler.cpp:452-456`)旁加`m_ioThreadShutdown = true; m_ioCv.notify_all(); if (m_ioThread.joinable()) m_ioThread.join();`。
- [ ] **步骤5:实现`VmSchedulerIo.cpp`**(完整骨架,≤500行):

```cpp
/*---
    VmSchedulerIo.cpp — IO parking concern of the context scheduler
    (v0.13.0 net): ParkForIo/WakeForIo, the interest table, and the
    dedicated lazy-started poller thread. The thread body mirrors
    TimerThreadMain (VmSchedulerTimer.cpp:100-130 — copy its cv/GIL
    discipline exactly): outside the GcWorldStop
    rendezvous by construction, never runs bytecode, holds the global
    lock only while mutating scheduler state — released around
    NetPollerWait (plan ruling R6 caps the wait so a newly registered
    interest is picked up within one slice).
---*/
#include "VmScheduler.h"
#include "net/NetPlatform.h"
#include <algorithm>

namespace nlang {

//v0.13.0 net: the poll slice (R6) — bounds the latency of picking up
//newly registered interest while the wait is fd-driven.
static constexpr int kIoPollSliceMs = 10;

void VmScheduler::ParkForIo(int id, int fd, uint32_t events,
    std::chrono::steady_clock::time_point deadline, bool hasDeadline) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedIo;
    m_ioWaits.push_back(IoWaitEntry{id, fd, events, deadline, hasDeadline});
    ReleaseHeldIfCurrent(id);
    NotifyGateWaiters();
    if (!m_ioThreadStarted) {
        m_ioThreadStarted = true;
        m_ioThread = std::thread([this] { IoPollerThreadMain(); });
    }
    m_ioCv.notify_all();
}

void VmScheduler::WakeForIo(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx || ctx->state != ContextState::ParkedIo)
        return;
    ctx->state = ContextState::Runnable;
    if (ctx->framesParked)
        m_runQueue.push_back(ctx->id);
    //else: the parking carrier enqueues in the same critical section
    //that stores the frames (K7 handshake, same as WakeForMutex).
}

void VmScheduler::RemoveIoWaitsForFd(int fd) {
    GilEnsureGuard g(*this);
    m_ioWaits.erase(
        std::remove_if(m_ioWaits.begin(), m_ioWaits.end(),
            [fd](const IoWaitEntry& e) { return e.fd == fd; }),
        m_ioWaits.end());
    m_ioCv.notify_all();   //the poller re-syncs interest without it
}

//The poller thread body (lazy start, see ParkForIo). Empty interest:
//cv long sleep (releases the global lock). With interest: interest is
//re-synced to the poller set each loop (idempotent add/mod), the wait
//timeout is the nearest deadline capped at kIoPollSliceMs, and the
//wait itself runs with the global lock RELEASED.
void VmScheduler::IoPollerThreadMain() {
    void* poller = NetPollerCreate();
    for (;;) {
        GilEnsureGuard gil(*this);
        m_ioCv.wait(gil.Lock(), [this] {
            return m_ioThreadShutdown || !m_ioWaits.empty();
        });
        if (m_ioThreadShutdown)
            break;
        SyncPollerInterest(poller);
        const int timeoutMs = NextIoTimeoutMs();
        std::vector<NetPollEvent> evs;       //thread-local scratch
        gil.Release();                       //wait without the GIL
        NetPollerWait(poller, evs, timeoutMs);
        GilEnsureGuard g2(*this);            //harvest under the GIL
        HarvestIoEvents(poller, evs);
        g2.Release();
        m_runQueueCv.notify_all();           //woken contexts: wake carriers
    }
    NetPollerClose(poller);
}
```

  - `SyncPollerInterest`:对每个`fd>=0`条目`NetPollerAdd/Mod`(按`interest`字段去重);对已消失fd`NetPollerDel`。
  - `NextIoTimeoutMs`:无截止条目→`kIoPollSliceMs`;有→`min(最近截止-now, kIoPollSliceMs)`,下限0。
  - `HarvestIoEvents(poller, evs)`:对每个事件找条目(`fd`+事件位匹配)→`ctx->state==ParkedIo`才唤醒(陈旧条目丢弃,防livelock,同`ExpireDueTimers`),唤醒后从`m_ioWaits`移除该条目;再扫`hasDeadline && now>=deadline`的条目→唤醒+移除。

- [ ] **步骤6:`src/vm/CMakeLists.txt`加`VmSchedulerIo.cpp`;跑绿**`ctest --test-dir build-dev -C Release -R net_scheduler`(经gate.sh,30s内)。
- [ ] **步骤7:提交**`git commit -m "feat(vm): IO parking state and the dedicated poller thread"`。

**验收判据:**`net_scheduler_tests`绿;既有全量ctest无回归(`ctest --test-dir build-dev -C Release`经gate.sh PASS)。

---

### 任务4:net包骨架——异常、字节助手、TcpListener绑定探针

**文件:**
- 新建:`stdlib/net.n`(本任务先落异常+字节助手+TcpListener声明;完整面见步骤6注)
- 新建:`src/vm/IntrinsicsNetSupport.cpp`
- 修改:`include/nlang/vm/Intrinsics.h`、`src/compiler/builder/BindingTable.cpp`、`src/vm/VmExecutorIntrinsics.cpp`、`src/vm/VmExecutor.cpp`、`src/vm/VmExecutor.h`、`src/vm/CMakeLists.txt`、`tests/test_langservice/test_symbol_index.cpp`、`tests/e2e/manifest.txt`
- 测试:`tests/e2e/stdlib_net_bytes_roundtrip.n`、`tests/e2e/stdlib_net_tcp_bind_probe.n`

**接口:**
- 消费:任务2槽表、任务3ParkForIo。
- 产出:`ExecuteIntrinsicNet(uint16_t, uint16_t, uint8_t*, uint8_t*)`(任务5/8/9在此挂分支)、`MintNetHandle(int16_t classIdx, int32_t slotHandle)`、`m_socketExcClassIdx/m_timeoutExcClassIdx`、四个句柄类idx缓存、`ReadByteArrayArg/WriteByteArrayToBuffer`字节数组助手、ID 174..183已定义。

- [ ] **步骤1:写失败e2e**`tests/e2e/stdlib_net_bytes_roundtrip.n`:

```nlang
import io;
import net;

int main() {
    byte[] data = net.toBytes("hello");
    if (data.length() != 5) { return 1; }
    if (data[0] != 104) { return 2; }
    string s = net.fromBytes(data);
    if (s != "hello") { return 3; }
    try {
        throw SocketException("boom");
    } catch (SocketException e) {
        if (e.message != "boom") { return 4; }
    }
    try {
        throw TimeoutException("late");
    } catch (IOException e) {
        if (e.message != "late") { return 5; }   //TimeoutException : IOException
    }
    io.print("ok");
    return 0;
}
```

`tests/e2e/stdlib_net_tcp_bind_probe.n`:

```nlang
import io;
import net;

int main() {
    TcpListener l = TcpListener.bind("127.0.0.1", 0);
    if (l.localPort() <= 0) { return 1; }
    if (l.localAddress() != "127.0.0.1") { return 2; }
    l.close();
    io.print("ok");
    return 0;
}
```

manifest追加:

```
stdlib_net_bytes_roundtrip 0 ok
stdlib_net_tcp_bind_probe 0 ok
```

- [ ] **步骤2:跑e2e确认失败**(`import net`未知包,ncc报错)。
- [ ] **步骤3:`Intrinsics.h`追加完整ID块**(174..220一次到位,后续任务不再动此文件):

```cpp
//v0.13.0 net package (net library design §5/§6): the appended block
//174..220 (173 is Array.toString, taken by the merged array wave).
//Static-face rows live in the binding table's derived face
//projection; method keys are "Class.method". 221 = next fresh id.
static constexpr uint16_t INTR_SocketException_Ctor      = 174;
static constexpr uint16_t INTR_TimeoutException_Ctor     = 175;
static constexpr uint16_t INTR_Net_ToBytes               = 176;
static constexpr uint16_t INTR_Net_FromBytes             = 177;
static constexpr uint16_t INTR_Net_Ping                  = 178;
//TcpListener (bind is the static face: host, port from slot 0/1).
static constexpr uint16_t INTR_TcpListener_Bind          = 179;
static constexpr uint16_t INTR_TcpListener_Accept        = 180;
static constexpr uint16_t INTR_TcpListener_LocalAddress  = 181;
static constexpr uint16_t INTR_TcpListener_LocalPort     = 182;
static constexpr uint16_t INTR_TcpListener_Close         = 183;
//TcpStream (connect is the static face: host, port, timeoutMs).
static constexpr uint16_t INTR_TcpStream_Connect         = 184;
static constexpr uint16_t INTR_TcpStream_Read            = 185;
static constexpr uint16_t INTR_TcpStream_Write           = 186;
static constexpr uint16_t INTR_TcpStream_Close           = 187;
static constexpr uint16_t INTR_TcpStream_ShutdownWrite   = 188;
static constexpr uint16_t INTR_TcpStream_IsClosed        = 189;
static constexpr uint16_t INTR_TcpStream_SetNoDelay      = 190;
static constexpr uint16_t INTR_TcpStream_SetKeepAlive    = 191;
static constexpr uint16_t INTR_TcpStream_SetSendBufferSize = 192;
static constexpr uint16_t INTR_TcpStream_SetRecvBufferSize = 193;
static constexpr uint16_t INTR_TcpStream_PeerAddress     = 194;
static constexpr uint16_t INTR_TcpStream_PeerPort        = 195;
static constexpr uint16_t INTR_TcpStream_LocalAddress    = 196;
static constexpr uint16_t INTR_TcpStream_LocalPort       = 197;
//UdpSocket (bind is the static face: host, port).
static constexpr uint16_t INTR_UdpSocket_Bind            = 198;
static constexpr uint16_t INTR_UdpSocket_SendTo          = 199;
static constexpr uint16_t INTR_UdpSocket_RecvFromRaw     = 200;
static constexpr uint16_t INTR_UdpSocket_LastRecvAddress = 201;
static constexpr uint16_t INTR_UdpSocket_LastRecvPort    = 202;
static constexpr uint16_t INTR_UdpSocket_Connect         = 203;
static constexpr uint16_t INTR_UdpSocket_Send            = 204;
static constexpr uint16_t INTR_UdpSocket_Recv            = 205;
static constexpr uint16_t INTR_UdpSocket_SetBroadcast    = 206;
static constexpr uint16_t INTR_UdpSocket_JoinMulticast   = 207;
static constexpr uint16_t INTR_UdpSocket_LeaveMulticast  = 208;
static constexpr uint16_t INTR_UdpSocket_SetMulticastTtl = 209;
static constexpr uint16_t INTR_UdpSocket_SetTtl          = 210;
static constexpr uint16_t INTR_UdpSocket_LocalAddress    = 211;
static constexpr uint16_t INTR_UdpSocket_LocalPort       = 212;
static constexpr uint16_t INTR_UdpSocket_PeerAddress     = 213;
static constexpr uint16_t INTR_UdpSocket_PeerPort        = 214;
static constexpr uint16_t INTR_UdpSocket_Close           = 215;
//IcmpSocket (bind is the static face: host).
static constexpr uint16_t INTR_IcmpSocket_Bind           = 216;
static constexpr uint16_t INTR_IcmpSocket_SendTo         = 217;
static constexpr uint16_t INTR_IcmpSocket_RecvFromRaw    = 218;
static constexpr uint16_t INTR_IcmpSocket_LastRecvAddress = 219;
static constexpr uint16_t INTR_IcmpSocket_Close          = 220;
```

- [ ] **步骤4:`BindingTable.cpp`追加net行块**(在String face块之后)+`s_HandleClassNames`追加四个类名:

```cpp
	//v0.13.0 net package (net library design §5): the exception ctors
	//follow the /2 default-cause rule; free functions key "net.name";
	//methods key "Class.method". recvFrom's out parameters are a
	//script-layer wrapper (plan ruling R2) — the raw natives carry the
	//data. write's count default keeps one row (the default stages).
	{"net", "SocketException/2",          INTR_SocketException_Ctor},
	{"net", "TimeoutException/2",         INTR_TimeoutException_Ctor},
	{"net", "net.toBytes",                INTR_Net_ToBytes},
	{"net", "net.fromBytes",              INTR_Net_FromBytes},
	{"net", "net.ping",                   INTR_Net_Ping},
	{"net", "TcpListener.bind",           INTR_TcpListener_Bind},
	{"net", "TcpListener.accept",         INTR_TcpListener_Accept},
	{"net", "TcpListener.localAddress",   INTR_TcpListener_LocalAddress},
	{"net", "TcpListener.localPort",      INTR_TcpListener_LocalPort},
	{"net", "TcpListener.close",          INTR_TcpListener_Close},
	{"net", "TcpStream.connect",          INTR_TcpStream_Connect},
	{"net", "TcpStream.read",             INTR_TcpStream_Read},
	{"net", "TcpStream.write",            INTR_TcpStream_Write},
	{"net", "TcpStream.close",            INTR_TcpStream_Close},
	{"net", "TcpStream.shutdownWrite",    INTR_TcpStream_ShutdownWrite},
	{"net", "TcpStream.isClosed",         INTR_TcpStream_IsClosed},
	{"net", "TcpStream.setNoDelay",       INTR_TcpStream_SetNoDelay},
	{"net", "TcpStream.setKeepAlive",     INTR_TcpStream_SetKeepAlive},
	{"net", "TcpStream.setSendBufferSize",   INTR_TcpStream_SetSendBufferSize},
	{"net", "TcpStream.setRecvBufferSize",   INTR_TcpStream_SetRecvBufferSize},
	{"net", "TcpStream.peerAddress",      INTR_TcpStream_PeerAddress},
	{"net", "TcpStream.peerPort",         INTR_TcpStream_PeerPort},
	{"net", "TcpStream.localAddress",     INTR_TcpStream_LocalAddress},
	{"net", "TcpStream.localPort",        INTR_TcpStream_LocalPort},
	{"net", "UdpSocket.bind",             INTR_UdpSocket_Bind},
	{"net", "UdpSocket.sendTo",           INTR_UdpSocket_SendTo},
	{"net", "UdpSocket.recvFromRaw",      INTR_UdpSocket_RecvFromRaw},
	{"net", "UdpSocket.lastRecvAddress",  INTR_UdpSocket_LastRecvAddress},
	{"net", "UdpSocket.lastRecvPort",     INTR_UdpSocket_LastRecvPort},
	{"net", "UdpSocket.connect",          INTR_UdpSocket_Connect},
	{"net", "UdpSocket.send",             INTR_UdpSocket_Send},
	{"net", "UdpSocket.recv",             INTR_UdpSocket_Recv},
	{"net", "UdpSocket.setBroadcast",     INTR_UdpSocket_SetBroadcast},
	{"net", "UdpSocket.joinMulticast",    INTR_UdpSocket_JoinMulticast},
	{"net", "UdpSocket.leaveMulticast",   INTR_UdpSocket_LeaveMulticast},
	{"net", "UdpSocket.setMulticastTtl",  INTR_UdpSocket_SetMulticastTtl},
	{"net", "UdpSocket.setTtl",           INTR_UdpSocket_SetTtl},
	{"net", "UdpSocket.localAddress",     INTR_UdpSocket_LocalAddress},
	{"net", "UdpSocket.localPort",        INTR_UdpSocket_LocalPort},
	{"net", "UdpSocket.peerAddress",      INTR_UdpSocket_PeerAddress},
	{"net", "UdpSocket.peerPort",         INTR_UdpSocket_PeerPort},
	{"net", "UdpSocket.close",            INTR_UdpSocket_Close},
	{"net", "IcmpSocket.bind",            INTR_IcmpSocket_Bind},
	{"net", "IcmpSocket.sendTo",          INTR_IcmpSocket_SendTo},
	{"net", "IcmpSocket.recvFromRaw",     INTR_IcmpSocket_RecvFromRaw},
	{"net", "IcmpSocket.lastRecvAddress", INTR_IcmpSocket_LastRecvAddress},
	{"net", "IcmpSocket.close",           INTR_IcmpSocket_Close},
```

```cpp
const char* const s_HandleClassNames[] = {
	"ByteStream", "FileStream", "List", "Dict", "Thread",
	"Mutex", "Channel", "Future", "Semaphore", "Promise",
	"ReentrantMutex",
	"AtomicInt",
	//v0.13.0 net handle classes (declared in stdlib/net.n).
	"TcpListener", "TcpStream", "UdpSocket", "IcmpSocket",
};
```

- [ ] **步骤5:`stdlib/net.n`本任务落地面**(异常+字节助手+TcpListener;其余声明随任务5/8/9追加,最终全文见步骤6注记):

```nlang
// net.n — 网络通信包:TCP、UDP、ICMP。需显式`import net;`。
// 阻塞方法在虚拟线程上停泊(不占载体),timeoutMs=0表示无限等待。
// 失败抛SocketException,超时抛TimeoutException(同属IOException家族)。

// 底层socket错误:连接被拒、对端断开、socket已关闭、选项设置失败等。
const class SocketException : IOException {
    public native SocketException(string message, Exception cause = null);
}

// 带timeoutMs的操作在截止前未完成。
const class TimeoutException : IOException {
    public native TimeoutException(string message, Exception cause = null);
}

// string是字节容器;这两个助手在文本与字节数组间转换(UTF-8字节序列,
// 不经string方法面)。
native byte[] toBytes(string s);
native string fromBytes(byte[] data);

// TCP监听端点。bind后accept循环接受连接;close停止监听。
class TcpListener {
    // 监听host:port;port=0由系统分配,经localPort()读回。
    public static native TcpListener bind(string host, int port);
    // 便利重载:监听所有接口的port。
    public static TcpListener bind(int port) {
        return bind("0.0.0.0", port);
    }
    // 接受下一个连接;停泊直至有连接或超时(timeoutMs=0无限等)。
    public native TcpStream accept(int timeoutMs = 0);
    public native string localAddress();
    public native int localPort();
    public native void close();
}
```

- [ ] **步骤6:`IntrinsicsNetSupport.cpp`**(家族入口+字节助手+句柄铸造;≤500行):

```cpp
/*---
    IntrinsicsNetSupport.cpp — the net intrinsic family entry (net
    library design §6): the id-range dispatch, the byte[] <-> string
    conversions, handle-class minting, and the shared slot-argument
    validation every net TU uses. The exception ctors ride the shared
    chain in VmExecutorIntrinsics.cpp (like every other Exception
    subclass); the raise paths use the by-name class caches.
---*/
#include "VmExecutor.h"
#include "net/NetPlatform.h"
#include <cstring>

namespace nlang {

bool VmExecutor::ExecuteIntrinsicNet(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    if (intrinsicId < INTR_SocketException_Ctor
        || intrinsicId > INTR_IcmpSocket_Close)
        return false;
    NetPlatformInit();   //R4: process-level once, first net touch
    //The ctors are handled by the shared chain before the family
    //cascade; the range entry still claims them so an unknown-id
    //throw never masks a missing arm.
    if (intrinsicId == INTR_SocketException_Ctor
        || intrinsicId == INTR_TimeoutException_Ctor)
        return false;   //the shared Exception chain owns these arms
    if (intrinsicId == INTR_Net_ToBytes || intrinsicId == INTR_Net_FromBytes
        || intrinsicId == INTR_Net_Ping)
        return ExecuteIntrinsicNetSupport(intrinsicId, callParamBase,
                                          locals, pResult);
    if (intrinsicId <= INTR_TcpStream_LocalPort)
        return ExecuteIntrinsicNetTcp(intrinsicId, callParamBase,
                                      locals, pResult);
    if (intrinsicId <= INTR_UdpSocket_Close)
        return ExecuteIntrinsicNetUdp(intrinsicId, callParamBase,
                                      locals, pResult);
    return ExecuteIntrinsicNetIcmp(intrinsicId, callParamBase,
                                   locals, pResult);
}
```

`ExecuteIntrinsicNetSupport`本任务实现toBytes/fromBytes(ping在任务9):

```cpp
//net.toBytes(string) — free function: the string handle rides slot 0.
//Mints a rank-1 byte array (FindScalarArrayTypeIdx mirrors the
//host-native minting path, VmExecutorNativeCallbacks.cpp).
bool VmExecutor::IntrinsicNetToBytes(uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    int32_t strHandle;
    std::memcpy(&strHandle, locals + callParamBase, sizeof(strHandle));
    const std::string content = StrVal(strHandle);   //"" for handle 0
    int32_t len = static_cast<int32_t>(content.size());
    const uint16_t arrayTypeIdx = FindScalarArrayTypeIdx(RTK_Byte, 1);
    const int32_t arrIdx = AllocArrayOnHeap(arrayTypeIdx, 1, &len);
    //Payload cells are int32 each (ArrayElemCells(RTK_Byte) == 1);
    //write by index — AllocArrayOnHeap may have reallocated the store.
    auto& rec = m_structHeap[static_cast<size_t>(arrIdx)];
    const size_t base = ArrayHeaderCells(1);
    for (int32_t i = 0; i < len; ++i)
        rec[base + static_cast<size_t>(i)] =
            static_cast<uint8_t>(content[static_cast<size_t>(i)]);
    std::memcpy(pResult, &arrIdx, sizeof(arrIdx));
    return true;
}
```

(fromBytes对称:校验`m_slotKinds[idx]==RTK_Array`、rank==1、elemKind∈{RTK_Byte,RTK_UByte},视图实参经`ReadNetBufferArg`同款偏移解析,逐cell取低字节拼`std::string`,`MintNewString`。)

共享助手(供各net TU):

```cpp
//Reads the byte-array argument at a slot: validates handle/rank/elem
//kind, returns the payload base + count. Post-merge slice syntax
//makes views legal arguments: resolve through ArrayViewOffsetCell
//(base = ArrayHeaderCells(1) + view offset, count = the view's
//elemCount; consumer pattern VmExecutorNativeCallbacks.cpp:141); a
//view with stride != 1 raises SocketException("view stride must be
//1"). Raises SocketException on a null or wrong-shaped argument
//(the API surface is byte[]/ubyte[]).
void VmExecutor::ReadNetBufferArg(uint16_t slotOffset, uint16_t callParamBase,
    uint8_t* locals, int32_t& outArrIdx, size_t& outBase, int32_t& outCount);
//Copies count payload cells into a net buffer's cells at base (read
//direction fills them after recv); base already carries the view
//offset when the argument was a view.
void VmExecutor::WriteNetBufferCells(int32_t arrIdx, size_t base,
    const uint8_t* src, int count);
//Mints a net handle object: AllocClassOnHeap + __handle cell 1 = the
//slot handle (MintThreadObject shape, IntrinsicsThread.cpp:265).
int32_t VmExecutor::MintNetHandle(int16_t classIdx, int32_t slotHandle);
//Reads the receiver's slot handle (slot 0 = this) and looks the slot
//up; raises SocketException("socket closed") on a stale/closed slot.
NetSocketSlot* VmExecutor::NetSlotFromReceiver(uint16_t callParamBase,
    uint8_t* locals, int32_t& outHandle);
//True when the slot handle no longer resolves to a live slot (the
//close-race check inside the park loops).
bool VmExecutor::IsNetSlotClosed(int32_t handle);
//Raises SocketException through the by-name class cache. Wraps
//RaiseNlangException(m_socketExcClassIdx, msg) (VmExecutor.h:1985,
//[[noreturn]]); message prefix "net: ".
[[noreturn]] void VmExecutor::RaiseNetSocketError(const std::string& msg);
//Reads the int argument at slot offset n (stride kFrameSlotBytes from
//callParamBase; the memcpy pattern of the string intrinsics).
int VmExecutor::ReadNetIntArg(int slotOffset, uint16_t callParamBase,
    uint8_t* locals);
//Writes an int result into pResult (memcpy pattern of the host
//natives).
void VmExecutor::WriteNetResultInt(uint8_t* pResult, int value);
//Parks the current context on fd+events until ready/deadline/close:
//EnsureSuspensionAllowed() + m_spScheduler->ParkForIo(selfId, ...) +
//RequestPark(ParkSignal::Reason::Io) — the two-phase handshake
//template of IntrinsicsChannel.cpp:270-345; on the wake the intrinsic
//re-enters right after RequestPark and re-looks-up the slot.
void VmExecutor::ParkForCurrentIo(int fd, uint32_t events,
    std::chrono::steady_clock::time_point deadline, bool hasDeadline);
```

Deadline arithmetic is two free helpers (declared in `VmExecutor.h`'s
net section, defined in `IntrinsicsNetSupport.cpp` — no executor state
needed):

```cpp
//timeoutMs <= 0 -> no deadline: the sentinel time_point::max().
std::chrono::steady_clock::time_point IoDeadline(int timeoutMs);
//True once now >= deadline (the max() sentinel never passes).
bool DeadlinePassed(std::chrono::steady_clock::time_point deadline);
```

- [ ] **步骤7:`VmExecutor.h/.cpp`接线**:
  - `VmExecutor.h`成员:`NetSocketTable m_netSlots;`、`int16_t m_socketExcClassIdx = -1; int16_t m_timeoutExcClassIdx = -1; int16_t m_tcpListenerClassIdx = -1; int16_t m_tcpStreamClassIdx = -1; int16_t m_udpSocketClassIdx = -1; int16_t m_icmpSocketClassIdx = -1;`(类idx一律`int16_t`,与`RaiseNlangException(int16_t, ...)`及既有`cacheExcClass(name, int16_t*)`一致)、方法声明(上面助手+`ExecuteIntrinsicNet/…Support/…Tcp/…Udp/…Icmp`+两个截止自由函数)。
  - `VmExecutor.cpp`初始化区(`cacheExcClass`处,~90-130行):`cacheExcClass("SocketException", &m_socketExcClassIdx); cacheExcClass("TimeoutException", &m_timeoutExcClassIdx);`+四个类idx仿Thread/Future缓存(VmExecutor.cpp:105-109):`int idx = module.FindClassBareOrUniqueSuffix("TcpListener"); m_tcpListenerClassIdx = (idx >= 0) ? static_cast<int16_t>(idx) : -1;`等。
  - `VmExecutorIntrinsics.cpp`:Exception ctor链追加`|| intrinsicId == INTR_SocketException_Ctor || intrinsicId == INTR_TimeoutException_Ctor`;级联在`ExecuteIntrinsicString`之后加`if (ExecuteIntrinsicNet(intrinsicId, callParamBase, locals, pResult)) return;`。
  - `src/vm/CMakeLists.txt`加`IntrinsicsNetSupport.cpp`(Tcp/Udp/Icmp三个TU随任务5/8/9加,本任务先以Support TU内临时stub三个返回false——任务5起逐个替换)。
- [ ] **步骤8:R1探针**:编译`stdlib_net_tcp_bind_probe.n`。若`static native`成员绑定失败(resolver拒静态native或绑定表无行),按裁定R1退路:`ExprResolverBuiltinClasses.cpp`的`s_Faces`加四行(`{"net","TcpListener","bind",INTR_TcpListener_Bind,2}`等)并从绑定表删去`TcpListener.bind`/`TcpStream.connect`/`UdpSocket.bind`/`IcmpSocket.bind`四行(派生行接管),net.n声明不变。探针判据=e2e绿。
- [ ] **步骤9:`test_symbol_index.cpp`更新**(合并后master基线):三处`CHECK(index.size() == 175)`→`178`(tensor塌缩后master基线175,net +3),注释补`net 3 (toBytes/fromBytes/ping)`;`TestCompletion`(test_symbol_index.cpp:89-94)`CHECK(namespaces.size() == 5)`→`6`并插入`CHECK(namespaces[3] == "net")`(tensor/thread后移至[4]/[5]);`CompleteNamespaces`列表`{fs, io, math, net, tensor, thread}`(字母序)。
- [ ] **步骤10:跑绿**:e2e两条+`ctest -R symbol_index`+全量ctest无回归(gate.sh)。
- [ ] **步骤11:实现`INTR_TcpListener_Bind/LocalAddress/LocalPort/Close`**(IntrinsicsNetSupport.cpp内,任务5移入Tcp TU):bind=NetResolve取首endpoint→`NetOpenTcpStream(ep.family)`+NetBind+NetListen(64)+槽表Mint+MintNetHandle(host="::"时NetFamily::Inet6+IPV6_V6ONLY=0双栈);失败`RaiseNlangException(m_socketExcClassIdx, "net: bind " + host + ":" + port + " failed: " + NetLastErrorText())`。close=`MarkClosed`+`RemoveIoWaitsForFd`+唤醒(本任务无停泊者,直接关)。
- [ ] **步骤12:提交**`git commit -m "feat(net): package skeleton — exceptions, byte helpers, TcpListener bind"`。
- [ ] **步骤13:编译器崩溃回归守卫**(独立提交,任务5前必须落地):dev树ncc在数组创建尺寸为**调用表达式**时内部崩溃(0xC0000005,`byte[s.length()]`即崩;复现`temp/review_probe_ovlZ.n`),master的subscript/creation重写(78898435族)已顺带修复——2026-11-06以master ncc/nvm实测探针编译运行皆绿(nvm rc=2),前置合并后该崩溃不复存在,本步骤不再改编译器。net的e2e/示例/手册片段常用`byte[s.length()]`惯用法,且docs审计真实编译片段,故新增回归e2e`tests/e2e/array_size_call_expr.n`钉死该惯用法(manifest加一行):

```nlang
import io;
class Box {
    public int v = 4;
    public int size() { return v; }
}
int main() {
    string s = "hi";
    byte[] b = byte[s.length()];
    Box x = Box();
    int[] a = int[x.size()];
    if (b.length() != 2 || a.length() != 4) { return 1; }
    io.print("ok");
    return 0;
}
```

跑绿提交`git commit -m "test(compiler): regression e2e for call-expression array creation size"`(纯测试提交)。

**验收判据:**两条e2e绿;array_size_call_expr绿;symbol_index绿;全量ctest绿。

---

### 任务5:ABI v11——nowMs/parkForIo/moduleState/wakeIoWaiters/currentContextId/submitWork + VmWorkPool + blocking臂停泊解卷 + io包脚本面

**文件:**
- 新建:`src/vm/VmWorkPool.h`/`VmWorkPool.cpp`(通用原生工作线程池,r1补注4)、`tests/test_vm/test_work_pool.cpp`(池单测)
- 修改:`include/nlang/vm/NativeHost.h`(v10→v11,回调表尾追加六项)、`src/vm/VmExecutorNativeHost.cpp`(InitNativeHost接线+六实现)、`src/vm/VmExecutorBlockingNative.cpp`(park-pending处置,R7)、`src/vm/VmExecutor.h/.cpp`(moduleState表成员)、`src/vm/VmScheduler.h/.cpp`(上下文`m_ioParkArmed`/`m_ioParkPending`字段,WakeForIo置armed,新增`FinishIoPark`,R7;VmWorkPool成员+`SubmitWork`惰性访问+收尾join,r1补注4)、`src/vm/VmSchedulerIo.cpp`(`WakeForIo`置armed;`FinishIoPark`实现)、`src/vm/CMakeLists.txt`(加VmWorkPool.cpp)、`src/native/io/io_native.cpp`(注册nowMs/parkForIo/toBytes/fromBytes)、`stdlib/io.n`(四声明)、`stdlib/net.n`(删公开toBytes/fromBytes,r1补注5)、`tests/e2e/manifest.txt`、`tests/CMakeLists.txt`(注册test_work_pool)、`tests/test_compiler/test_library_index.cpp`/`tests/test_langservice/test_symbol_index.cpp`(177→179;io 15→19、net 2→0;namespaces 6→5)
- 改名:`tests/e2e/stdlib_net_bytes_roundtrip.n`→`stdlib_io_bytes_roundtrip.n`(import io,io.toBytes/fromBytes;manifest同步改名)
- 测试:`tests/e2e/stdlib_io_park_now.n`、`tests/e2e/stdlib_io_park_now.flags`、`tests/test_vm/test_work_pool.cpp`

**接口:**
- 消费:`VmScheduler::ParkForIo/WakeForIo`(任务3已提交)。
- 产出:ABI v11六项回调(R7停泊契约、R8 moduleState形态、R13 wakeIoWaiters、R14 currentContextId、submitWork工作线程池,r1补注4)、`VmWorkPool`(per-调度器惰性起停,`Submit(fn,arg,ackFd,priority)`,worker无GIL跑fn、完成向ackFd写一字节,两级优先级队列)、`io.nowMs()`/`io.parkForIo(fd,events,deadlineMs)`/`io.toBytes(string)`/`io.fromBytes(byte[])`脚本面、每上下文armed/pending标志。

- [ ] **步骤1:写失败e2e**`stdlib_io_park_now.n`(审查焦点5的io层前哨:停泊让出载体):

```nlang
import io;
import thread;

class Ticker {
    public int ticks = 0;
    public void run() {
        long end = io.nowMs() + 300;
        while (io.nowMs() < end) { ticks = ticks + 1; }
    }
}

int main() {
    long t0 = io.nowMs();
    Ticker tk = Ticker();
    Thread t = thread.spawn(tk.run);
    io.parkForIo(-1, 0, t0 + 200);      //纯截止停泊,约200ms
    long dt = io.nowMs() - t0;
    if (dt < 150) { return 1; }          //停泊确实等待了
    thread.join(t);
    if (tk.ticks <= 0) { return 2; }     //停泊期间载体跑了另一上下文
    io.print("ok");
    return 0;
}
```

manifest追加`stdlib_io_park_now 0 ok`;新建`stdlib_io_park_now.flags`内容`--carriers=1`(审查焦点5:停泊必须让出载体——仅单载体下该断言才有意义,多载体时另一载体自然能跑Ticker)。

- [ ] **步骤2:跑e2e确认失败**(io.nowMs/io.parkForIo未声明)。
- [ ] **步骤3:写失败单测**`tests/test_vm/test_work_pool.cpp`(`tests/CMakeLists.txt`注册,同test_net_scheduler模式链nlang_vm):

```cpp
// test_work_pool.cpp — VmWorkPool unit tests (r1补注4: generic native
// worker pool, the concurrent library's native engine layer).
#include "VmWorkPool.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using namespace nlang;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

//1) submit runs fn on a worker; Shutdown joins every in-flight worker.
static void TestRunsAndJoins()
{
    VmWorkPool pool;
    std::atomic<int> counter{0};
    for (int i = 0; i < 8; ++i) {
        pool.Submit([](void* arg) {
            (*static_cast<std::atomic<int>*>(arg)).fetch_add(1);
        }, &counter, -1, 0);
    }
    pool.Shutdown();                       //joins all workers
    CHECK(counter.load() == 8);
}

//2) priority: one worker, a blocker occupies it while a normal and a
//   high job queue up; after release the high job runs first.
static void TestPriorityOrder()
{
    VmWorkPool pool(1);                    //single worker for determinism
    std::atomic<bool> releaseBlocker{false};
    std::mutex orderMutex;
    std::string order;
    auto append = [&](char c) {
        std::lock_guard<std::mutex> lk(orderMutex);
        order.push_back(c);
    };
    pool.Submit([&releaseBlocker](void*) {
        while (!releaseBlocker.load()) { std::this_thread::yield(); }
    }, nullptr, -1, 0);
    pool.Submit([&append](void*) { append('N'); }, nullptr, -1, 0);
    pool.Submit([&append](void*) { append('H'); }, nullptr, -1, 1);
    releaseBlocker.store(true);
    pool.Shutdown();
    CHECK(order == "HN");
}

//3) lazy start: no worker before the first submit; workers exist after.
//   (The ack byte itself rides a caller-owned socket fd — exercised by
//   task 7's net_jobs integration, not by this no-ack unit test.)
static void TestLazyStart()
{
    VmWorkPool pool;
    CHECK(pool.Running() == 0);
    std::atomic<bool> ran{false};
    pool.Submit([](void* arg) {
        *static_cast<std::atomic<bool>*>(arg) = true;
    }, &ran, -1, 0);
    CHECK(pool.Running() > 0);
    pool.Shutdown();
    CHECK(ran.load());
}

int main()
{
    TestRunsAndJoins();
    TestPriorityOrder();
    TestLazyStart();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
```

- [ ] **步骤4:跑单测确认失败**(`VmWorkPool.h`不存在,编译失败)。
- [ ] **步骤5:`NativeHost.h`升v11**:`NLANG_HOST_ABI_VERSION` 10u→11u;`NativeHostFunctions`表尾追加六项(注释写明契约):

```c
    //v11: monotonic milliseconds, same clock domain as the VM IO poller
    //deadlines (steady_clock epoch; not wall time).
    long long (*nowMs)(NativeHost* self);
    //v11: register fd interest + optional absolute deadline (steady-clock
    //ms; hasDeadline=0 means none), then unwind the calling native.
    //Returns 0 on the first entry (interest registered; the native MUST
    //return immediately — the VM parks the context and releases the
    //carrier) and 1 on re-entry after wake (the VM re-executes the
    //CallNative with the same staged arguments; retry the operation).
    //fd < 0 = pure deadline park. Only callable from a blocking-channel
    //native (the GIL is released on entry; the host re-takes it).
    //Note: the armed flag is set by ANY WakeForIo wake, including
    //interpreter-path parks (e.g. thread.sleep) that never called
    //parkForIo; a stale armed makes the next blocking-native's first
    //parkForIo return 1 (one extra retry). Harmless by contract — the
    //caller re-checks its condition on every return — do not "fix" it
    //by clearing armed on other wake paths.
    int (*parkForIo)(NativeHost* self, int fd, unsigned events,
        long long deadlineMs, int hasDeadline);
    //v11: per-executor host-managed module state slot keyed by tag. The
    //host calls ctor() once per executor, stores the pointer with dtor,
    //and runs dtor at executor teardown (before the module unloads —
    //modules never unload). Later calls return the stored pointer.
    void* (*moduleState)(NativeHost* self, const char* tag,
        void* (*ctor)(void), void (*dtor)(void*));
    //v11: unregister poller interest for fd and wake every context parked
    //on it (socket close path — the module cannot reach the VM poller
    //directly). Callable without the GIL; the host takes it internally.
    void (*wakeIoWaiters)(NativeHost* self, int fd);
    //v11: id of the context executing the current blocking-native call
    //(stable across park/re-entry — the net module's per-direction waiter
    //guard needs it to tell its own re-entry from a foreign concurrent
    //claim, plan ruling R14). Only callable from a blocking-channel
    //native (the staged calling context).
    int (*currentContextId)(NativeHost* self);
    //v11: run fn(arg) on the host's generic native worker pool (workers
    //hold no GIL); on completion the pool sends one byte to ackFd (a
    //socket fd owned by the caller; -1 = no ack). priority: 0 normal,
    //1 high (two-level queue — a slow getaddrinfo never starves short
    //jobs; libuv uv_queue_work precedent). The pool starts lazily and
    //is joined at scheduler teardown (after all carriers stop, the R8
    //precondition). Callable from any channel; returns 0.
    int (*submitWork)(NativeHost* self, void (*fn)(void*), void* arg,
        int ackFd, int priority);
```

- [ ] **步骤6:宿主实现**(`VmExecutorNativeHost.cpp`+`VmWorkPool.{h,cpp}`+`VmScheduler`):`InitNativeHost`接线六项;
  - `NativeNowMs`:`duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()`——与`VmSchedulerIo`截止同域(其`ParkForIo`收`steady_clock::time_point`)。
  - `NativeParkForIo`(无GIL进入):`GilEnsureGuard`内——`if (ctx->m_ioParkArmed) { ctx->m_ioParkArmed=false; return 1; }` → `m_spScheduler->ParkForIo(selfId, fd, events, deadlinePoint, hasDeadline)`(已提交:登记兴趣+置ParkedIo+惰性起轮询线程)→ `ctx->m_ioParkPending=true; return 0;`(首入只置pending;armed由`WakeForIo`置,R7协议)。
  - `NativeModuleState`:执行器`m_moduleStates`(`std::map<std::string, std::pair<void*, void(*)(void*)>>`,专用mutex);缺tag→`ctor()`入表;析构路径(执行器收尾,`RemoveNativeRoots`同段;全部载体已停止——R8前提)按tag字典序跑dtor清表(R8)。
  - `NativeWakeIoWaiters`(R13,无GIL进入):`GilEnsureGuard`内先从`m_ioWaits[fd]`收集等待者上下文id并注销兴趣(已提交`RemoveIoWaitsForFd`只注销不唤醒——按其既有签名取id列表或改为收集后删),再逐个`WakeForIo(id)`(置armed+按K7握手入队)。
  - `NativeCurrentContextId`(R14,blocking通道内):返`m_spScheduler->CurrentContextId()`(`VmExecutorBlockingNative.cpp`既有暂存记录即当前调用上下文)。
  - `NativeSubmitWork`:`m_spScheduler->SubmitWork(fn, arg, ackFd, priority)`(r1补注4);池队列锁独立于GIL(blocking通道无GIL调用安全;执行器在native调用期间存活)。
  - `VmWorkPool`(新建,`src/vm/`):`DEFAULT_WORK_POOL_THREADS=4`(magic number常量化;libuv `uv_queue_work`默认先例);`Submit(fn, arg, ackFd, priority)`两级`std::deque`(priority 1先出)+`std::condition_variable`,首次Submit惰性起worker;worker循环:出队→无GIL跑`fn(arg)`→`ackFd>=0`时`send(ackFd,"\1",1,0)`(部分写循环,错误忽略);`Shutdown()`置停旗标+`notify_all`+join(幂等,析构调用;停旗标只在队列排空后生效——worker把已提交作业出队跑完才退出,`TestRunsAndJoins`依赖此;net侧后果:调度器收尾join时排队作业先跑完,收尾至多拖到一次解析超时,dtor的cv等待已覆盖);`Running()`返已起worker数(惰性起停可观测)。`VmScheduler`持`std::unique_ptr<VmWorkPool>`+专用mutex,`SubmitWork`惰性构造;调度器收尾join池——与轮询线程收尾同模板(停旗标+notify+join),且满足R8前提(全部载体停止后)。
- [ ] **步骤7:blocking臂停泊处置**(`VmExecutorBlockingNative.cpp`,R7):停泊分支插入已提交K6重取尾段`FinishBlockingNative`的`StopRequested`检查之后——其既有顺序`BeginExecuting`→`owner.Acquire`重取GIL→`RemoveNativeRoots`→generation失配检查→`StopRequested`检查不可动摇(已提交注释明文:`BeginExecuting`先于GIL重取,否则垃圾收集停世界的会合持GIL挂条件变量等待、最后到达的收集者需GIL而死锁;pending读取置于两检查之后,僵尸载体路径上下文已析构、先读悬空)。分支体(GIL已持有):读`ctx->m_ioParkPending`→置位则清位,调调度器新方法`FinishIoPark(selfId)`(R7——取`ParkContextFrames`判定半段;帧已由`ParkFramesForBlockingNative`移入`ctx->frames`,该方法首步`frames.clear()`不可复用;GIL已持有、不再重取):同一临界区内`ctx->state==ParkedIo`→置`framesParked=true`(唤醒路径见framesParked即入队);已`Runnable`(唤醒抢在停泊登记前)→直接入队。随后**不**`RestoreStolenFrames`、**不**写回ret(帧留在`ctx->frames`,既有跨挂起根集覆盖;停泊上下文无在飞结果)→返回枚举`Parked`;调度器对`Parked`的处理与内建intrinsic`RequestPark`路径一致(载体回循环,上下文已ParkedIo)。唤醒:`WakeForIo`(已提交,本轮改置`m_ioParkArmed`)→调度器迁移上下文(framesParked握手保证入队)→重执行`OP_CallNative`→`RunBlockingNative`重新暂存参数(脚本帧未动,绝对截止点穿越重入稳定)→native再调`parkForIo`见armed→清→返1→重试syscall。
- [ ] **步骤8:`io_native.cpp`注册**:`reg(registry,"io","nowMs",&IoNowMs)`(经`host->nowMs()`,不触VM态,普通通道)+`regBlocking(registry,"io","parkForIo",&IoParkForIo)`(直通`host->parkForIo`,返0即回)+`reg(registry,"io","toBytes",&IoToBytes)`/`reg(registry,"io","fromBytes",&IoFromBytes)`(纯转换,普通通道;语义照已提交VM intrinsic(任务4台账)改写为ABI形态——`ArgString`→`newArray`(RTK_Byte)填cell;fromBytes对称:`ArgArray`视图(校验rank==1、elemKind∈{RTK_Byte,RTK_UByte})逐cell取低字节拼`std::string`→`MintNewString`)。`stdlib/io.n`追加四声明:

```nlang
// 单调毫秒时钟(与 io.parkForIo 的截止同域;起点为系统启动,非墙上时钟)。
native long nowMs();

// 停泊当前虚拟线程直至 fd 就绪(events:1=读,2=写)或到达绝对截止
// (deadlineMs<=0 表示无截止)。fd<0 为纯截止停泊(定时唤醒)。
// 唤醒后本调用返回;调用方复查条件,未满足可再次停泊。
native void parkForIo(int fd, int events, long deadlineMs);

// string是字节容器;这两个助手在文本与字节数组间转换(UTF-8字节序列,
// 不经string方法面)。r1补注5:自net包迁入——通用字节转换非网络专属。
native byte[] toBytes(string s);
native string fromBytes(byte[] data);
```

  同步:`stdlib/net.n`删除`toBytes`/`fromBytes`两公开声明(r1补注5;BindingTable的net.toBytes/fromBytes行留待任务6整族删除——net.n已不声明,行暂为死代码无害);`git mv tests/e2e/stdlib_net_bytes_roundtrip.n tests/e2e/stdlib_io_bytes_roundtrip.n`,用例改`import io;`+`io.toBytes`/`io.fromBytes`(原仅用这两助手,去`import net;`),`manifest.txt`行同步改名。
- [ ] **步骤9:跑绿e2e+全量ctest**(ABI升版后四既有模块同构建重编,探针自然通过;`test_work_pool`RED→GREEN,ctest 149→150)。
- [ ] **步骤10:提交**`git commit -m "feat(vm): ABI v11 IO parking and work pool callbacks with blocking-native park re-entry and io package surface"`。

**验收判据:**`stdlib_io_park_now`绿(单载体);`test_work_pool`绿(全量ctest 149→150);全量ctest绿;`test_library_index`io计数15→19、net 2→0(总数177→179);`test_symbol_index`namespaces 6→5(net暂无自由函数,任务6重写net.n后回升6)。

---

### 任务6:nlang_net模块骨架——搬迁 + VM解耦 + net.n重构

**文件:**
- 移动:`src/vm/net/NetPlatform.h`→`src/native/net/net_platform.h`(剥轮询部分)、`NetPlatformInternal.h`→`net_platform_internal.h`、`NetPlatformWindows.cpp`→`net_platform_windows.cpp`、`NetPlatformWindowsIcmp.cpp`→`net_platform_icmp_windows.cpp`、`NetPlatformPosix.cpp`→`net_platform_posix.cpp`、`NetSocketTable.{h,cpp}`→`src/native/net/`(git mv保历史)
- 新建:`src/vm/IoPoller.h`/`IoPollerWindows.cpp`/`IoPollerPosix.cpp`(轮询抽象从NetPlatform拆出,wepoll/epoll留VM)、`src/native/net/net_native.cpp`(模块入口)、`src/native/net/net_state.{h,cpp}`(宿主托管每执行器状态:槽表+last-error;作业表/自管道随任务7加入)
- 删除:`src/vm/IntrinsicsNetSupport.cpp`
- 修改:`include/nlang/vm/Intrinsics.h`(删174..220整块,下一空闲回174)、`src/compiler/builder/BindingTable.cpp`(删net行+`s_HandleClassNames`四类)、`src/vm/VmExecutorIntrinsics.cpp`(删net级联+ctor链)、`src/vm/VmExecutor.h/.cpp`(删net成员/类idx缓存/异常idx/收尾关fd/include)、`src/vm/VmSchedulerIo.cpp`(include改`IoPoller.h`)、`src/vm/CMakeLists.txt`(删net TU+IntrinsicsNetSupport+iphlpapi;wepoll留)、`src/native/CMakeLists.txt`(加net+target_sources+链ws2_32/iphlpapi)、`stdlib/net.n`(全文重写)、`tests/CMakeLists.txt`(test_net_platform/test_net_slots改编译模块源、不链nlang_vm;新增test_module_state)、`tests/test_vm/test_net_platform.cpp`(轮询集测试移入test_net_scheduler.cpp)、`tests/test_compiler/test_library_index.cpp`、`tests/test_langservice/test_symbol_index.cpp`
- 测试:`tests/test_vm/test_module_state.cpp`(新);既有`stdlib_io_bytes_roundtrip`(任务5改名)/`stdlib_net_tcp_bind_probe`作搬迁回归网

**接口:**
- 消费:任务5的ABI v11六项。
- 产出:`nlang_net`模块(注册`_nowMs`/`_lastErrorText`/`_tcpBind`/`_tcpLocalAddress`/`_tcpLocalPort`/`_tcpClose`;字节助手已归io包,任务9随分帧追加内部`_toBytes`/`_fromBytes`)、`net_state`状态对象(R8)、net.n新形态(纯脚本异常类+脚本类持int句柄+内部namespace native,R10/R11/R12)。

- [ ] **步骤1:回归基线**——跑两条net e2e确认绿(VM路径,搬迁前快照)。
- [ ] **步骤2:拆轮询抽象**:`IoPoller.h`声明`IoPollerCreate/Add/Mod/Del/Wait/Close`+事件常量(已提交`NetPoller*`六函数改前缀为`IoPoller*`、签名不变——调度器六者全用:Add失败回退Mod、轮询线程收尾Close);定位注释写明**通用fd就绪设施**(net是第一个消费者;管道/进程stdio等持fd原生库后续复用;regular file不可poll、将来fs异步走线程池——libuv/Go/Loom同此,spec r1补注①);`IoPollerWindows.cpp`承接`NetPlatformWindows.cpp`的wepoll实现段,`IoPollerPosix.cpp`承接epoll骨架段;`VmSchedulerIo.cpp`的`#include "net/NetPlatform.h"`改`#include "IoPoller.h"`;`test_net_scheduler.cpp`并入原platform测试的轮询集用例。
- [ ] **步骤3:git mv上述七文件入模块**(五个平台层文件+`NetSocketTable.{h,cpp}`);`net_platform.h`删轮询声明;模块内命名空间保持`nlang::`;`net_platform_windows.cpp`补`NetSocketPair(int fds[2])`(回环TCP对,wepoll只认socket——R9);`net_slots.h`头注释加锁依据改写为"由`NetExecState::m_mutex`保护(r1下blocking native无GIL,R9)"——原"VM全局锁"前提随intrinsic通道废弃。
- [ ] **步骤4:`net_state.{h,cpp}`**:

```cpp
// Per-executor module state, host-managed through ABI v11 moduleState
// (design §6, plan ruling R8). Holds the socket slot table and the
// last-error text; the job table and self-pipe join in task 7. The
// dtor closes every still-open fd (executor teardown = D12 backstop);
// from task 7 it also waits for in-flight jobs before freeing (R9).
// m_mutex guards m_slots and m_lastErrorText: r1 blocking natives run
// with the GIL released, so the committed "all calls under the GIL"
// table invariant no longer holds (plan ruling R9). Lock order:
// m_mutex -> GIL (parkForIo/wakeIoWaiters take the GIL internally).
struct NetExecState {
    std::mutex m_mutex;
    NetSocketTable m_slots;
    std::string m_lastErrorText;
};
inline NetExecState* NetState(NativeHost* host) {
    return static_cast<NetExecState*>(host->moduleState(
        host, "nlang.net.v1", &NetStateCtor, &NetStateDtor));
}
```

(调用形态已按`NativeHost.h`核实:回调为平铺函数指针、首参`self`,故`host->moduleState(host, …)`;tag含版本号,状态布局变更时递增。)
- [ ] **步骤5:`net_native.cpp`模块入口**:`NLANG_DEFINE_NATIVE_INIT`注册——`regBlocking`:`_nowMs`(返host->nowMs,占blocking通道与其余net一致免混排)/`_lastErrorText`/`_tcpBind`/`_tcpLocalAddress`/`_tcpLocalPort`/`_tcpClose`。bind/close实现从`IntrinsicsNetSupport.cpp`迁出:改C ABI形态(`ArgString/ReturnInt/Raise`→错误码返回,R10;不再Raise——异常映射归脚本)。
- [ ] **步骤6:删VM耦合**(逐文件):`Intrinsics.h`删174..220块(注释"221=下一空闲"改回"174=下一空闲");`BindingTable.cpp`删`{"net",...}`全部行+`s_HandleClassNames`的TcpListener/TcpStream/UdpSocket/IcmpSocket四行;`VmExecutorIntrinsics.cpp`删`ExecuteIntrinsicNet`级联与ctor链两行;`VmExecutor.h/.cpp`删`ExecuteIntrinsicNet*`/`IntrinsicNet*`/`ReadNetBufferArg`等net助手声明与实现、`m_netSlots`成员、`m_tcpListenerClassIdx`等四类idx、`cacheNetClass`/`cacheExcClass`的net条目、`#include "net/NetSocketTable.h"`、收尾遍历关fd段;`src/vm/CMakeLists.txt`删`IntrinsicsNetSupport.cpp`/`net/*`六TU/`iphlpapi`(ws2_32留——wepoll需要)。
- [ ] **步骤7:`src/native/CMakeLists.txt`**:`set(NLANG_NATIVE_MODULES math io fs tensor net)`;`target_sources(nlang_net PRIVATE net/net_state.cpp net/net_platform_windows.cpp net/net_platform_icmp_windows.cpp)`(POSIX分支换`net_platform_posix.cpp`);`target_link_libraries(nlang_net PRIVATE ws2_32 iphlpapi)`(Windows);include加`include`+`src/native`;install行加nlang_net。
- [ ] **步骤8:ctest重定向**:`test_net_platform`/`test_net_slots`改为直接编译`src/native/net/*.cpp`(不链nlang_vm,链ws2_32/iphlpapi;include`src/native`+`include`);新增`test_module_state`:进程内两执行器(先`Runtime::StaticInit()`)各经字节码跑`net.TcpListener.bind`+`close`,断言两执行器句柄值互不干扰(独立槽表)、执行器A析构后其端口可被重新bind(dtor已关fd)。
- [ ] **步骤9:`stdlib/net.n`全文重写**:

```nlang
// net.n — 网络通信包:TCP、UDP、ICMP。需显式`import net;`。
// 阻塞方法在虚拟线程上停泊(不占载体),timeoutMs=0表示无限等待。
// 失败抛SocketException,超时抛TimeoutException(同属IOException家族)。

// 底层socket错误:连接被拒、对端断开、socket已关闭、选项设置失败等。
const class SocketException : IOException {
    public SocketException(string message, Exception cause = null) {
        super(message, cause);
    }
}

// 带timeoutMs的操作在截止前未完成。
const class TimeoutException : IOException {
    public TimeoutException(string message, Exception cause = null) {
        super(message, cause);
    }
}

// ---- 内部原语(nlang_net模块注册;下划线名不属公开面) ----
// 单调毫秒(与VM轮询截止同域);脚本层据此换算绝对截止点(R12)。
native long _nowMs();
// 最近一次平台错误的文本(错误码-4时拼入异常消息)。
native string _lastErrorText();

// 内部:错误码映射异常(-1关闭/-2超时/-3并发违例/-4平台错误/-5参数)。
void _throwNet(int code, string context) {
    if (code == -2) { throw TimeoutException("net: " + context + " timed out"); }
    if (code == -1) { throw SocketException("net: socket closed"); }
    if (code == -3) { throw SocketException(
        "net: another operation is parked on this socket"); }
    if (code == -4) { throw SocketException(
        "net: " + context + " failed: " + _lastErrorText()); }
    throw SocketException("net: " + context + " failed");
}

// (字节助手toBytes/fromBytes已移入io包,r1补注5;脚本便捷面用下方
//  内部_toBytes/_fromBytes,随任务9分帧一并注册)

// ---- ICMP 便利面(实现随任务11落地) ----
// 发送 Echo Request 并等待 Echo Reply,返回往返时延毫秒。
// 超时抛 TimeoutException;主机无法解析或不可达抛 SocketException。
// Windows 走 IcmpSendEcho2(免管理员),Linux 走 DGRAM ICMP socket。
native int ping(string host, int timeoutMs = 5000);

// ---- TCP ----
native int _tcpBind(string host, int port);
native string _tcpLocalAddress(int handle);
native int _tcpLocalPort(int handle);
native int _tcpClose(int handle);

// TCP监听端点。bind后开始监听;close停止监听。
class TcpListener {
    private int m_handle;
    public TcpListener(int handle) { m_handle = handle; }

    // 监听host:port;port=0由系统分配,经localPort()读回。
    public static TcpListener bind(string host, int port) {
        int h = _tcpBind(host, port);
        if (h < 0) { _throwNet(h, "bind " + host + ":" + port.toString()); }
        return TcpListener(h);
    }
    // 便利重载:监听所有接口的port。
    public static TcpListener bind(int port) {
        return bind("0.0.0.0", port);
    }
    // 接受下一个连接;停泊直至有连接或超时(实现随任务7落地)。
    // accept返回TcpStream,声明见任务7。
    public string localAddress() {
        string s = _tcpLocalAddress(m_handle);
        if (s == null) { _throwNet(-4, "localAddress"); }
        return s;
    }
    public int localPort() {
        int p = _tcpLocalPort(m_handle);
        if (p < 0) { _throwNet(p, "localPort"); }
        return p;
    }
    public void close() {
        int r = _tcpClose(m_handle);
        if (r < 0) { _throwNet(r, "close"); }
    }
}
```

(探针:若`super(message, cause)`双参委托编译失败——`super_with_args.n`仅证双参super于普通类——退路=`super(message)`+cause存本类字段并在`message()`覆写拼接;两路e2e判据相同。)
- [ ] **步骤10:索引计数同步**:`test_library_index.cpp`与`test_symbol_index.cpp`计数179→**187**(net 0→8:+ping/_nowMs/_lastErrorText/_throwNet/_tcpBind/_tcpLocalAddress/_tcpLocalPort/_tcpClose;io四项已随任务5的177→179入账;类方法与类声明不入计数——io的ByteStream方法先例),注释同步;`test_symbol_index`namespaces 5→6(net自由函数回归,列表插入net字母序[3],tensor/thread后移[4]/[5])。
- [ ] **步骤11:跑绿**:两条net e2e(模块路径)+全量ctest(含重定向两目标+test_module_state)+`test_library_index`/`test_symbol_index`;e2e全量跑一遍确认无串扰。
- [ ] **步骤12:提交**`git commit -m "refactor(net): move the net package out of the VM into the nlang_net native module"`。

**验收判据:**两条e2e在模块路径绿(回归网);VM内`grep -ri "net" src/vm/ include/nlang/vm/Intrinsics.h`仅剩`VmSchedulerIo`/`IoPoller`族与注释;索引187;全量ctest绿。

---

### 任务7:TCP核心——accept/connect/read/write/close + 解析作业池

**文件:**
- 新建:`src/native/net/net_tcp.cpp`、`src/native/net/net_jobs.{h,cpp}`(作业表+自管道ack;工作线程池归VM的VmWorkPool,经submitWork提交,R9/r1补注4)
- 修改:`stdlib/net.n`(TcpStream+accept)、`src/native/net/net_state.{h,cpp}`(作业表+自管道入状态对象,析构关管道)、`src/native/CMakeLists.txt`(两TU)、`tests/e2e/manifest.txt`
- 测试:`tests/e2e/stdlib_net_tcp_echo.n`、`stdlib_net_tcp_connect_refused.n`、`stdlib_net_tcp_read_timeout.n`、`stdlib_net_tcp_localhost.n`、`stdlib_net_tcp_close_wakes_reader.n`、`stdlib_net_tcp_concurrent_reader.n`、`stdlib_net_tcp_write_closed.n`、`stdlib_net_tcp_ipv6.n`

**接口:**
- 消费:任务6的net_state/句柄形态/R10码表/R12绝对截止;任务5的parkForIo重入契约。
- 产出:内部native `_tcpAccept`/`_tcpResolveStart`/`_tcpResolveWait`/`_tcpResolveEndpoint`/`_tcpResolveRelease`/`_tcpConnectTry`/`_tcpRead`/`_tcpWrite`/`_tcpShutdownWrite`/`_tcpIsClosed`/`_tcpPeerAddress`/`_tcpPeerPort`(任务8补四选项setter)、`NetJobTable`+submitWork提交形态(作业跑VM的VmWorkPool;任务11 ICMP复用作业通道,priority=1)。

- [ ] **步骤1:写失败e2e**(八条,完整代码):

`stdlib_net_tcp_echo.n`:

```nlang
import io;
import thread;
import net;

class EchoHandler {
    public TcpStream conn;
    public void run() {
        byte[] buf = byte[16];
        int n = conn.read(buf, 5000);
        conn.write(buf, n);
        conn.close();
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    int port = srv.localPort();
    EchoHandler h = EchoHandler();
    //connect先行:listen backlog让内核先完成握手,accept随即取到挂起
    //连接;若accept在前,主上下文停泊等待、connect永不执行。
    TcpStream cli = TcpStream.connect("127.0.0.1", port, 5000);
    h.conn = srv.accept(5000);
    Thread t = thread.spawn(h.run);
    cli.write(io.toBytes("ping"));
    //本任务只有native read/write(readExactly随任务9):4字节缓冲恰好
    //容纳回环单段echo,fromBytes(整缓冲)即整消息。
    byte[] buf = byte[4];
    int n = cli.read(buf, 5000);
    if (n != 4 || io.fromBytes(buf) != "ping") { return 1; }
    cli.close();
    thread.join(t);
    srv.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_tcp_connect_refused.n`:

```nlang
import io;
import net;

int main() {
    // Port 1 on loopback: nothing listens, connect is refused.
    try {
        TcpStream s = TcpStream.connect("127.0.0.1", 1, 2000);
        s.close();
        return 1;
    } catch (SocketException e) {
        io.print("ok");
        return 0;
    }
}
```

`stdlib_net_tcp_read_timeout.n`(审查焦点1):

```nlang
import io;
import net;

int main() {
    // The client's read must time out against a silent peer; the
    // late-data variant proves the deadline never masks data that
    // did arrive.
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    int port = srv.localPort();
    TcpStream cli = TcpStream.connect("127.0.0.1", port, 5000);
    TcpStream c1 = srv.accept(5000);
    byte[] buf = byte[8];
    try {
        cli.read(buf, 300);
        return 1;
    } catch (TimeoutException e) {
        // expected
    }
    // Late data: send BEFORE the deadline, read with a generous
    // timeout — the data must win over the deadline bookkeeping.
    c1.write(io.toBytes("x"));
    int n = cli.read(buf, 3000);
    if (n != 1 || buf[0] != 120) { return 2; }
    cli.close();
    c1.close();
    srv.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_tcp_localhost.n`:

```nlang
import io;
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("localhost", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    cli.write(io.toBytes("hi"));
    byte[] buf = byte[2];
    if (c.read(buf, 5000) != 2 || io.fromBytes(buf) != "hi") { return 1; }
    cli.close(); c.close(); srv.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_tcp_close_wakes_reader.n`(审查焦点2):

```nlang
import io;
import thread;
import net;

class BlockedReader {
    public TcpStream s;
    public int result = 0;
    public void run() {
        byte[] buf = byte[8];
        try {
            s.read(buf, 0);          //parks forever
            result = 1;              //should not reach
        } catch (SocketException e) {
            result = 2;              //closed wake
        }
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    BlockedReader r = BlockedReader();
    r.s = c;
    Thread t = thread.spawn(r.run);
    thread.sleep(100);               //reader is parked
    c.close();                       //close wakes the parked reader
    thread.join(t);
    if (r.result != 2) { return 1; }
    cli.close(); srv.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_tcp_concurrent_reader.n`:

```nlang
import io;
import thread;
import net;

class Reader {
    public TcpStream s;
    public int outcome = 0;
    public void run() {
        byte[] buf = byte[8];
        try { s.read(buf, 3000); outcome = 1; }
        catch (SocketException e) { outcome = 2; }
        catch (TimeoutException e) { outcome = 3; }
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    Reader a = Reader(); a.s = c;
    Reader b = Reader(); b.s = c;
    Thread t1 = thread.spawn(a.run);
    thread.sleep(50);
    Thread t2 = thread.spawn(b.run);   //same direction, same socket
    thread.join(t1); thread.join(t2);
    //One parks (times out: 3), the other is rejected (2) — order
    //independent, both outcomes present.
    if (!((a.outcome == 3 && b.outcome == 2)
          || (a.outcome == 2 && b.outcome == 3))) { return 1; }
    c.close(); cli.close(); srv.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_tcp_write_closed.n`(审查焦点3):

```nlang
import io;
import thread;
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    c.close();                       //peer gone
    thread.sleep(100);               //let FIN land
    try {
        // A big write guarantees the first send hits the closed peer
        // (a tiny one may sit in the socket buffer — the loop then
        // surfaces EPIPE on the retry).
        cli.write(io.toBytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        cli.write(io.toBytes("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
        return 1;
    } catch (SocketException e) {
        io.print("ok");
        return 0;
    }
}
```

`stdlib_net_tcp_ipv6.n`(spec§三IPv6:`::`双栈监听+`[::1]`括号地址,D10;测试限`::1`回环):

```nlang
import io;
import net;

int main() {
    TcpListener srv = TcpListener.bind("::", 0);
    //connect先行:listen backlog让内核先完成握手,accept随即取到挂起
    //连接;若accept在前,主上下文停泊等待、connect永不执行。
    TcpStream cli = TcpStream.connect("[::1]", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    if (c.peerAddress() != "::1") { return 1; }
    cli.write(io.toBytes("v6"));
    byte[] buf = byte[8];
    int n = c.read(buf, 5000);
    c.write(buf, n);
    byte[] echo = byte[8];
    int m = cli.read(echo, 5000);
    if (m != 2 || io.fromBytes(echo) != "v6") { return 2; }
    cli.close();
    c.close();
    srv.close();
    io.print("ok");
    return 0;
}
```

manifest追加八行(均`... 0 ok`,connect_refused亦`0 ok`)。

- [ ] **步骤2:跑e2e确认失败**(TcpStream未声明)。
- [ ] **步骤3:`stdlib/net.n`追加TcpListener.accept+内部native声明+TcpStream**(公开面同spec§五;实现切分见裁定):

```nlang
native int _tcpAccept(int handle, long deadlineMs);
native int _tcpResolveStart(string host, int port);
native int _tcpResolveWait(int jobId, long deadlineMs);
native string _tcpResolveEndpoint(int jobId, int index);
native void _tcpResolveRelease(int jobId);
native int _tcpConnectTry(string address, int port, long deadlineMs);
native int _tcpRead(int handle, byte[] buf, long deadlineMs);
native int _tcpWrite(int handle, byte[] data, int count, long deadlineMs);
native int _tcpShutdownWrite(int handle);
native int _tcpIsClosed(int handle);
native string _tcpPeerAddress(int handle);
native int _tcpPeerPort(int handle);

// TCP字节流连接。connect/accept产出;read/write停泊;close释放。
class TcpStream {
    private int m_handle;
    public TcpStream(int handle) { m_handle = handle; }

    // 解析host(停泊于解析池)并按序尝试地址;timeoutMs覆盖
    // 解析+连接全程(0=不限)。
    public static TcpStream connect(string host, int port, int timeoutMs = 0) {
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        string ctx = "connect " + host + ":" + port.toString();
        int jobId = _tcpResolveStart(host, port);
        int count = _tcpResolveWait(jobId, deadline);   //停泊于自管道
        TcpStream result = null;
        if (count >= 0) {
            for (int i = 0; i < count; i = i + 1) {
                int h = _tcpConnectTry(_tcpResolveEndpoint(jobId, i), port, deadline);
                if (h >= 0) { result = TcpStream(h); break; }
                if (h == -2) { break; }    //截止已到:不再试下一地址
            }
        }
        _tcpResolveRelease(jobId);         //所有路径释放作业记录(R9)
        if (result != null) { return result; }
        if (count < 0) { _throwNet(count, ctx); }
        _throwNet(count == 0 ? -4 : -2, ctx);   //全败→平台错误;已过截止→超时
        return TcpStream(0);                    //不可达(上面必抛)
    }
    // 至多读满buf.length()字节;返回读到的字节数,0=对端已关闭(EOF)。
    public int read(byte[] buf, int timeoutMs = 0) {
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        int n = _tcpRead(m_handle, buf, deadline);
        if (n < 0) { _throwNet(n, "read"); }
        return n;
    }
    // 全写:内部循环直至count字节全部发出(默认整数组);返回发出数。
    public int write(byte[] data, int count = -1) {
        int n = (count >= 0) ? count : data.length();
        int r = _tcpWrite(m_handle, data, n, 0);
        if (r < 0) { _throwNet(r, "write"); }
        return r;
    }
    // 文本便捷面write(string)、readLine、readExactly随任务9追加
    // (脚本层方法,见任务9步骤2全文)。
    public void close() {
        int r = _tcpClose(m_handle);
        if (r < 0) { _throwNet(r, "close"); }
    }
    // 半关闭:发出FIN,读方向保持可用。
    public void shutdownWrite() {
        int r = _tcpShutdownWrite(m_handle);
        if (r < 0) { _throwNet(r, "shutdownWrite"); }
    }
    public bool isClosed() { return _tcpIsClosed(m_handle) == 1; }
    public string peerAddress() {
        string s = _tcpPeerAddress(m_handle);
        if (s == null) { _throwNet(-4, "peerAddress"); }
        return s;
    }
    public int peerPort() {
        int p = _tcpPeerPort(m_handle);
        if (p < 0) { _throwNet(p, "peerPort"); }
        return p;
    }
    public string localAddress() {
        string s = _tcpLocalAddress(m_handle);
        if (s == null) { _throwNet(-4, "localAddress"); }
        return s;
    }
    public int localPort() {
        int p = _tcpLocalPort(m_handle);
        if (p < 0) { _throwNet(p, "localPort"); }
        return p;
    }
}
```

(TcpListener类体内追加:`public TcpStream accept(int timeoutMs = 0) { long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0; int h = _tcpAccept(m_handle, deadline); if (h < 0) { _throwNet(h, "accept"); } return TcpStream(h); }`)

- [ ] **步骤4:`net_jobs.{h,cpp}`作业表+submitWork提交形态**(R9,r1补注4):

```cpp
/*---
    net_jobs — the blocking-OS-call offload of the net module (design
    §5/D5): DNS resolve and (task 11) ICMP echo jobs are submitted to
    the VM's generic native worker pool via host->submitWork — the pool
    is VM infrastructure (r1补注4); the module keeps only the job table
    and the self-pipe ack. ICMP uses priority 1 so a slow getaddrinfo
    never delays a ping (two-level queue, libuv precedent) without any
    module-private threads. Job records live in the per-executor
    NetExecState; the requesting context parks on the executor's
    self-pipe read end (parkForIo, ordinary fd interest — the VM poller
    harvests it). The worker fills the record under the table mutex;
    submitWork's ack byte (ackFd = self-pipe write end) then lands on
    the pipe — after fn returns, hence always after the fill. Record
    lifetime: the context releases on resume (including timeout), the
    worker marks done; the table frees only when both flags are set —
    a late completion never touches freed memory.
---*/
```

- `struct NetJobRecord { int kind; bool done, released; std::vector<std::string> endpoints; int rttMs; int err; }`+表:`Mint/Lookup/MarkDone(id)(填结果→解锁→通知cv;ack字节由VM池在fn返回后写)/Release(id)/FreeIfBoth`;自管道:`NetExecState`持`int selfPipeFds[2]`(经`NetSocketPair`建,非阻塞、close-on-exec;`selfPipeFds[1]`即submitWork的ackFd),析构关管道。
- 提交形态:worker fn=模块static C函数,经`host->submitWork(fn, argRecord, st->m_selfPipeFds[1], priority)`提交——`argRecord`为堆记录(持`NetExecState*`+jobId+host/port等,worker用毕释放);DNS priority=0、ICMP priority=1。worker无GIL跑`NetResolve(host,port,endpoints)`→经状态指针`MarkDone`(表mutex下填结果→通知cv)。跨执行器访问状态:argRecord存`NetExecState*`(提交时在调用载体上经moduleState解析——`NativeHost`是每次调用的栈上对象,存其指针调用返回即悬空,R9);执行器析构:dtor置shutdown并在cv上等所有在飞作业到done再释放状态——worker不写已释放内存(R9;池本身在调度器收尾join,晚于全部载体停止——R8前提)。良性竞态(审查第3轮Info):dtor的cv等待在done置位时(fn内、fn返回前)即被唤醒,可先于池的ack写关闭selfPipeFds——`send`到已关fd错误被忽略;Windows句柄复用的杂字节窗口极窄且收尾期无停泊等待者,电平重查契约兜底。
- `_tcpResolveStart`:字面量IP直接入记录(endpoints当场填,`MarkDone`+直写管道一字节——不经池,统一等待路径);否则`submitWork`(priority=0,见上);返jobId。`_tcpResolveWait`:循环`{ drain自管道读端(读到would-block); 状态mutex临界区内{查记录done→返count; 查截止→返-2; host->parkForIo(host, selfPipeRead, EV_READ, deadline, has)首入登记(锁序状态mutex→GIL,与R9等待方临界区同形态); 返0则return(重入再查)} }`(R9 drain纪律:水平触发防自旋;等待侧临界区镜像R9(审查第2轮):worker的done填充本在同一mutex下,"done已置"与"ack字节在登记之后才写"二必居其一——防一等待者的drain吃掉另一等待者的ack字节后,后者带着done=true睡到永久截止(timeoutMs=0);任务11`_ping`等待循环同此形态;wait从不释放记录——释放在脚本侧`_tcpResolveRelease`统一做,幂等,released/done齐则`FreeIfBoth`释放,worker迟到ack自然收敛,connect提前命中也不漏)。`_tcpResolveEndpoint`:越界返null。
- [ ] **步骤5:`net_tcp.cpp`**(每函数≤50行)。`_tcpRead`完整体(其余同形态):

```cpp
//Release the read claim if this call holds it (R9: table access under
//the state mutex; R5: the last release performs the deferred close).
static void NetReleaseRead(NetExecState* st, int32_t handle, bool claimed) {
    if (!claimed) { return; }
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_slots.ReleaseWaiter(handle, NetDir::Read);
}
//_tcpRead(handle, buf, deadlineMs): the park/retry loop lives inside
//the native (design §7): try recv, then the deadline, then claim +
//park (review focus 1: data always wins over expired bookkeeping).
//The slot's waiter field is the context id (committed form); this
//call claims it before parking and releases it on every exit path.
static void NetTcpRead(NativeHost* host, uint8_t* ret,
    const uint8_t* args, int argc)
{
    NetExecState* st = NetState(host);
    const int32_t handle = ArgInt(args, 0);
    NativeArrayView buf;
    ArgArray(host, args, 1, &buf);
    const long long deadline = ArgLong(args, 2);
    const int self = host->currentContextId(host);   //R14
    bool claimed = false;   //this call holds the read-direction waiter
    for (;;) {
        int fd = -1;
        {   //fd copied under the mutex: another carrier's Mint may
            //reallocate the slot vector while we run the syscall.
            std::lock_guard<std::mutex> g(st->m_mutex);
            NetSocketSlot* slot = st->m_slots.Lookup(handle);
            if (!slot) {   //null = stale or closed
                //Unconditional clear of our direction: a claim here is
                //either this context's own pre-park claim (releasing it
                //lets the R5 deferred close proceed) or a foreign one
                //the closer already woke — that waiter re-enters through
                //this same null branch without touching the fd, so
                //closing the fd now cannot race a parked syscall.
                st->m_slots.ReleaseWaiter(handle, NetDir::Read);
                ReturnInt(ret, kNetErrClosed);
                return;
            }
            fd = slot->fd;
            //Re-entry rebuilds this stack: adopt the claim that
            //survived the park. waiter==self can only be this call's
            //own claim (D11 keeps foreign readers out while it is set).
            claimed = (slot->readWaiter == self);
        }
        const int n = NetRecv(fd, buf.data, buf.elemCount);   //non-blocking
        if (n >= 0) {   //data or EOF (n == 0)
            NetReleaseRead(st, handle, claimed);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetReleaseRead(st, handle, claimed);
            NetRememberLastError(st);   //lastErrorText under the mutex (R9)
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (deadline > 0 && host->nowMs(host) >= deadline) {
            NetReleaseRead(st, handle, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        //Claim check, claim set and the parkForIo first-entry interest
        //registration share ONE state-mutex critical section (R9 lost-
        //wakeup window: the closer either flags closed before the claim
        //or sees the claim and wakes us after we park).
        std::lock_guard<std::mutex> g(st->m_mutex);
        NetSocketSlot* slot = st->m_slots.Lookup(handle);
        if (!slot) {   //lock held: release directly, not via the helper
            if (claimed) st->m_slots.ReleaseWaiter(handle, NetDir::Read);
            ReturnInt(ret, kNetErrClosed);
            return;
        }
        if (slot->readWaiter != 0 && slot->readWaiter != self) {
            if (claimed) st->m_slots.ReleaseWaiter(handle, NetDir::Read);
            ReturnInt(ret, kNetErrConcurrent);
            return;
        }
        st->m_slots.SetWaiter(handle, NetDir::Read, self);
        claimed = true;
        if (host->parkForIo(host, slot->fd, NET_EV_READ,
                deadline, deadline > 0) == 0) {
            return;   //first entry: unwinding under the claim
        }
    }
}
```

(waiter纪律(R14/R9):槽waiter字段=上下文id(已提交形态),模块经第五回调`host->currentContextId(host)`取得——跨停泊/重入稳定,用于区分"本调用重入"(id相等,claim延续)与"他人并发抢占"(非零且不等→-3);`claimed`栈局部标记"本调用已claim",重入后栈重建、须从槽采纳存活claim(`claimed=(slot->readWaiter==self)`,槽已关闭的分支则无条件清本方向——见草图注释),退出路径只清自己方向(防误清他人waiter破坏R5延迟关fd判定;审查第2轮:不采纳则"停泊后完成"的读/写/accept永久残留waiter,延迟关fd永不落地)。槽表与`lastErrorText`一切访问在`st->m_mutex`下;claim检查、置claim、parkForIo首入登记同一临界区,关闭方`MarkClosed`+读waiter标志同mutex、解锁后再调`wakeIoWaiters`——丢唤醒窗就此闭合。)`_tcpWrite`对称:写兴趣+循环至发满;EPIPE→`kNetErrPlatform`(文本connection lost)。`_tcpAccept`=监听fd读兴趣+`NetAccept`+新fd入槽+返新句柄(accept方向waiter同read纪律)。`_tcpConnectTry`=`NetOpenTcpStream`+非阻塞`NetConnect`→inprogress则写兴趣停泊循环→`NetConnectResult`(SO_ERROR)判成败,失败关fd返码;截止检查同read形态。
- [ ] **步骤6:其余方法契约**(≤10行/个):

| native | 行为 | 返回 |
| --- | --- | --- |
| _tcpClose | 状态mutex临界区{`MarkClosed`→读waiter标志}→解锁→`wakeIoWaiters(fd)`(宿主回调,见下);真正关fd由最后`ReleaseWaiter`执行(R5);无停泊者即关 | 0/-1 |
| _tcpShutdownWrite | `NetShutdownWrite` | 0/-4 |
| _tcpIsClosed | 槽缺或closed→1 | 1/0 |
| _tcpPeer*/_tcpLocal* | `NetPeer*/NetLocal*` | 文本/端口或-4 |

(`wakeIoWaiters`为任务5 ABI v11第四回调(R13):close native在blocking通道(无GIL)调用,宿主内部取GIL→从`m_ioWaits[fd]`收集等待者id并注销兴趣→逐个`WakeForIo`;真正关fd由最后`ReleaseWaiter`执行(R5)。)
- [ ] **步骤7:`CMakeLists.txt`加两TU;索引计数同步**(net新增文件面声明12个:`_tcpAccept/_tcpResolveStart/_tcpResolveWait/_tcpResolveEndpoint/_tcpResolveRelease/_tcpConnectTry/_tcpRead/_tcpWrite/_tcpShutdownWrite/_tcpIsClosed/_tcpPeerAddress/_tcpPeerPort`+TcpStream类方法不计数;187→199,`test_library_index`/`test_symbol_index`同步)。
- [ ] **步骤8:跑绿八条e2e**(单载体)+`--carriers=4`手跑echo一遍(不入manifest);全量ctest绿。
- [ ] **步骤9:提交**`git commit -m "feat(net): TCP listener/stream natives with parked IO and the DNS job pool"`。

**验收判据:**八条e2e绿;审查焦点1/2/3测试绿;全量ctest绿;索引199。

---

### 任务8:TCP选项面

**文件:**修改`src/native/net/net_tcp.cpp`、`stdlib/net.n`、`tests/e2e/manifest.txt`;测试`tests/e2e/stdlib_net_tcp_options.n`。

**接口:**消费任务1的`NetSet*`;产出`_tcpSetNoDelay/_tcpSetKeepAlive/_tcpSetSendBufferSize/_tcpSetRecvBufferSize`+四个脚本setter。

- [ ] **步骤1:写失败e2e**`stdlib_net_tcp_options.n`:

```nlang
import io;
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    cli.setNoDelay(true);
    cli.setKeepAlive(true);
    cli.setSendBufferSize(8192);
    cli.setRecvBufferSize(8192);
    // Options must not disturb the data path.
    cli.write(io.toBytes("opt"));
    byte[] buf = byte[4];
    if (c.read(buf, 5000) != 4) { return 1; }
    cli.close(); c.close(); srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:实现**:四native统一助手`ApplyOption/ApplyIntOption`(setsockopt失败→`kNetErrPlatform`+记last-error);net.n四脚本方法(`if (r < 0) _throwNet(r, "setNoDelay");`形态)。
- [ ] **步骤3:manifest加`stdlib_net_tcp_options 0 ok`;索引199→203;跑绿;提交**`git commit -m "feat(net): TCP socket option setters"`。

---

### 任务9:脚本层分帧——readLine/readExactly/write(string)

**文件:**修改`stdlib/net.n`、`src/native/net/net_native.cpp`(注册内部`_toBytes`/`_fromBytes`,r1补注5)、`tests/e2e/manifest.txt`;测试`tests/e2e/stdlib_net_tcp_readline.n`。

**接口:**消费native `read`/`write(byte[],int)`;产出三个脚本方法(HTTP分层地基)+模块内部字节转换`_toBytes`/`_fromBytes`(net.n脚本便捷面不能用`io.toBytes`——预置包互不import,r1补注5;约15行UTF-8转换在模块内重复,裁决见spec§六盘点表)。

- [ ] **步骤1:写失败e2e**`stdlib_net_tcp_readline.n`:

```nlang
import io;
import thread;
import net;

class FrameServer {
    public TcpStream conn;
    public void run() {
        string line1 = conn.readLine(5000);
        string line2 = conn.readLine(5000);
        if (line1 == "GET /x HTTP/1.0" && line2 == "") {
            conn.write("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        }
        conn.shutdownWrite();
        conn.close();
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    FrameServer s = FrameServer();
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    s.conn = srv.accept(5000);
    Thread t = thread.spawn(s.run);
    cli.write("GET /x HTTP/1.0\r\n");
    cli.write("X-Odd: 1\r\n\r\n");
    string status = cli.readLine(5000);
    if (status != "HTTP/1.0 200 OK") { return 1; }
    string hdr = cli.readLine(5000);
    hdr = cli.readLine(5000);          //blank line
    byte[] body = cli.readExactly(5, 5000);
    if (io.fromBytes(body) != "hello") { return 2; }
    cli.close();
    thread.join(t);
    srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:注册内部字节转换+net.n实现**:

`net_native.cpp`追加两条注册(普通通道,纯转换;实现语义照任务5的`IoToBytes`/`IoFromBytes`——`ArgString`→`newArray`(RTK_Byte)填cell/对称拼串):`reg(registry,"net","_toBytes",&NetToBytes);``reg(registry,"net","_fromBytes",&NetFromBytes);`。`stdlib/net.n`内部原语段追加:

```nlang
// 内部UTF-8字节转换(nlang_net模块注册;公开面是io.toBytes/fromBytes,
//  预置包互不import故脚本便捷面用内部副本,r1补注5)。
native byte[] _toBytes(string s);
native string _fromBytes(byte[] data);
```

```nlang
    // 文本便捷面:UTF-8编码后全写;返回发出的字节数。
    public int write(string s) {
        return this.write(_toBytes(s));
    }

    // 读一行:以'\n'结束,返回不含行尾的文本('\r'被剥离)。
    // 对端在行结束前关闭:返回已积累文本;零字节且EOF返回null。
    public string readLine(int timeoutMs = 0) {
        string acc = "";
        byte[] one = byte[1];
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        while (true) {
            int n = _tcpRead(m_handle, one, deadline);   //绝对截止跨多次读稳定
            if (n < 0) { _throwNet(n, "readLine"); }
            if (n == 0) {
                if (acc == "") { return null; }
                return acc;
            }
            if (one[0] == 10) {          //'\n'
                if (acc.length() > 0 && acc.endsWith("\r")) {
                    return acc.substring(0, acc.length() - 1);
                }
                return acc;
            }
            acc = acc + _fromBytes(one);
        }
    }

    // 读满count字节(HTTP定长体分帧);对端提前关闭抛SocketException。
    // read从缓冲首cell写入、无偏移入参:读入临时数组再逐cell搬运。
    public byte[] readExactly(int count, int timeoutMs = 0) {
        byte[] payload = byte[count];
        byte[] tmp = byte[count];
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        int got = 0;
        while (got < count) {
            int n = _tcpRead(m_handle, tmp, deadline);
            if (n < 0) { _throwNet(n, "readExactly"); }
            if (n == 0) {
                throw SocketException("net: connection closed after "
                    + got.toString() + " of " + count.toString()
                    + " bytes");
            }
            for (int i = 0; i < n; i = i + 1) {
                payload[got + i] = tmp[i];
            }
            got = got + n;
        }
        return payload;
    }
```

(逐字节readLine是v1直白实现,正确优先;性能线记入文档"高频分帧建议自带缓冲"。`acc.endsWith("\r")`/`substring(start,end)`(end不含)/`got.toString()`均为现有String面(已对码核实)。readLine/readExactly直调`_tcpRead`而非公开`read`:跨次读共享同一绝对截止点,公开read每次重算timeoutMs会无限续期。)

- [ ] **步骤3:manifest加`stdlib_net_tcp_readline 0 ok`;索引203→205(net自由函数+2:`_toBytes`/`_fromBytes`,`test_library_index`/`test_symbol_index`同步);跑绿;提交**`git commit -m "feat(net): script-layer framing — readLine, readExactly, text write"`。

---

### 任务10:UDP

**文件:**新建`src/native/net/net_udp.cpp`;修改`stdlib/net.n`、`src/native/CMakeLists.txt`、`tests/e2e/manifest.txt`;测试`stdlib_net_udp_echo.n`、`stdlib_net_udp_connected.n`、`stdlib_net_udp_multicast.n`。

**接口:**消费槽表/parkForIo;产出UDP全部内部native+R2脚本包装。

- [ ] **步骤1:写失败e2e**:

`stdlib_net_udp_echo.n`:

```nlang
import io;
import thread;
import net;

class UdpEcho {
    public UdpSocket s;
    public void run() {
        byte[] buf = byte[32];
        string from;
        int port = 0;
        int n = s.recvFrom(buf, out from, out port, 5000);
        s.sendTo(buf, from, port, n);  //回发来源;只发前n字节(R3)
    }
}

int main() {
    UdpSocket srv = UdpSocket.bind("127.0.0.1", 0);
    UdpEcho e = UdpEcho(); e.s = srv;
    Thread t = thread.spawn(e.run);
    UdpSocket cli = UdpSocket.bind("127.0.0.1", 0);
    cli.sendTo(io.toBytes("dgram"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[32];
    string from;
    int port = 0;
    int n = cli.recvFrom(buf, out from, out port, 5000);
    //fromBytes读整数组:先拷出恰好n字节的缓冲再比较。
    byte[] msg = byte[n];
    for (int i = 0; i < n; i = i + 1) { msg[i] = buf[i]; }
    if (n != 5 || io.fromBytes(msg) != "dgram") { return 1; }
    if (from != "127.0.0.1" || port != srv.localPort()) { return 2; }
    cli.close(); srv.close(); thread.join(t);
    io.print("ok");
    return 0;
}
```

`stdlib_net_udp_connected.n`:

```nlang
import io;
import net;

int main() {
    UdpSocket a = UdpSocket.bind("127.0.0.1", 0);
    UdpSocket b = UdpSocket.bind("127.0.0.1", 0);
    a.connect("127.0.0.1", b.localPort());
    a.send(io.toBytes("rel"));
    byte[] buf = byte[3];
    if (b.recv(buf, 5000) != 3 || io.fromBytes(buf) != "rel") { return 1; }
    if (a.peerAddress() != "127.0.0.1" || a.peerPort() != b.localPort()) { return 2; }
    a.close(); b.close();
    io.print("ok");
    return 0;
}
```

`stdlib_net_udp_multicast.n`:

```nlang
import io;
import net;

int main() {
    //绑通配地址:joinMulticast的接口=socket绑定地址(通配→INADDR_ANY),
    //与自发多播的默认出口接口一致,回环副本才会投递给本socket。
    UdpSocket s = UdpSocket.bind("0.0.0.0", 0);
    s.setMulticastTtl(1);
    s.joinMulticast("239.255.255.250");
    s.sendTo(io.toBytes("m"), "239.255.255.250", s.localPort());
    byte[] buf = byte[8];
    string from;
    int port = 0;
    int n = s.recvFrom(buf, out from, out port, 3000);
    s.leaveMulticast("239.255.255.250");
    s.close();
    if (n != 1) { return 1; }
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:net.n追加UdpSocket**(公开面同spec§五;`recvFrom`为脚本包装,R2):

```nlang
native int _udpBind(string host, int port);
native int _udpSendTo(int handle, byte[] data, int count, string host, int port);
native int _udpConnect(int handle, string host, int port);
native int _udpSend(int handle, byte[] data, int count);
native int _udpRecv(int handle, byte[] buf, long deadlineMs);
native int _udpRecvFrom(int handle, byte[] buf, long deadlineMs);
native string _udpLastRecvAddress(int handle);
native int _udpLastRecvPort(int handle);
native int _udpSetBroadcast(int handle, bool on);
native int _udpJoinMulticast(int handle, string group);
native int _udpLeaveMulticast(int handle, string group);
native int _udpSetMulticastTtl(int handle, int ttl);
native int _udpSetTtl(int handle, int ttl);
native string _udpLocalAddress(int handle);
native int _udpLocalPort(int handle);
native string _udpPeerAddress(int handle);
native int _udpPeerPort(int handle);
native int _udpClose(int handle);

class UdpSocket {
    private int m_handle;
    public UdpSocket(int handle) { m_handle = handle; }

    public static UdpSocket bind(string host = "0.0.0.0", int port = 0) {
        int h = _udpBind(host, port);
        if (h < 0) { _throwNet(h, "bind " + host + ":" + port.toString()); }
        return UdpSocket(h);
    }
    // count默认-1=全发整数组;≥0只发前count字节(R3,回发接收缓冲用)。
    public int sendTo(byte[] data, string host, int port, int count = -1) {
        int n = (count >= 0) ? count : data.length();
        int r = _udpSendTo(m_handle, data, n, host, port);
        if (r < 0) { _throwNet(r, "sendTo"); }
        return r;
    }
    // 收一条数据报,经out参数给出来源地址/端口;停泊直至有数据或超时。
    public int recvFrom(byte[] buf, out string peerAddress, out int peerPort,
                        int timeoutMs = 0) {
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        int n = _udpRecvFrom(m_handle, buf, deadline);
        if (n < 0) { _throwNet(n, "recvFrom"); }
        peerAddress = _udpLastRecvAddress(m_handle);
        peerPort = _udpLastRecvPort(m_handle);
        return n;
    }
    public void connect(string host, int port) {
        int r = _udpConnect(m_handle, host, port);
        if (r < 0) { _throwNet(r, "connect"); }
    }
    public int send(byte[] data, int count = -1) {
        int n = (count >= 0) ? count : data.length();
        int r = _udpSend(m_handle, data, n);
        if (r < 0) { _throwNet(r, "send"); }
        return r;
    }
    public int recv(byte[] buf, int timeoutMs = 0) {
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        int n = _udpRecv(m_handle, buf, deadline);
        if (n < 0) { _throwNet(n, "recv"); }
        return n;
    }
    public void setBroadcast(bool on) {
        int r = _udpSetBroadcast(m_handle, on);
        if (r < 0) { _throwNet(r, "setBroadcast"); }
    }
    public void joinMulticast(string group) {
        int r = _udpJoinMulticast(m_handle, group);
        if (r < 0) { _throwNet(r, "joinMulticast"); }
    }
    public void leaveMulticast(string group) {
        int r = _udpLeaveMulticast(m_handle, group);
        if (r < 0) { _throwNet(r, "leaveMulticast"); }
    }
    public void setMulticastTtl(int ttl) {
        int r = _udpSetMulticastTtl(m_handle, ttl);
        if (r < 0) { _throwNet(r, "setMulticastTtl"); }
    }
    public void setTtl(int ttl) {
        int r = _udpSetTtl(m_handle, ttl);
        if (r < 0) { _throwNet(r, "setTtl"); }
    }
    public string localAddress() {
        string s = _udpLocalAddress(m_handle);
        if (s == null) { _throwNet(-4, "localAddress"); }
        return s;
    }
    public int localPort() {
        int p = _udpLocalPort(m_handle);
        if (p < 0) { _throwNet(p, "localPort"); }
        return p;
    }
    // connect后的对端(未connect时抛SocketException)。
    public string peerAddress() {
        string s = _udpPeerAddress(m_handle);
        if (s == null) { _throwNet(-4, "peerAddress"); }
        return s;
    }
    public int peerPort() {
        int p = _udpPeerPort(m_handle);
        if (p < 0) { _throwNet(p, "peerPort"); }
        return p;
    }
    public void close() {
        int r = _udpClose(m_handle);
        if (r < 0) { _throwNet(r, "close"); }
    }
}
```

- [ ] **步骤3:`net_udp.cpp`**:bind/sendTo/connect/五setter/local*=close契约同TCP形态(setsockopt失败→-4+记last-error);`_udpRecvFrom`=读兴趣停泊循环(同`_tcpRead`形态)+`NetRecvFrom`填槽`lastPeerAddress/lastPeerPort`;`_udpSend`=`NetSendTo`(connected态`NetSend`)。
- [ ] **步骤4:manifest三行;索引205→223(18个`_udp*`);跑绿;提交**`git commit -m "feat(net): UDP socket with datagram parking and source reporting"`。

---

### 任务11:ICMP——IcmpSocket与ping

**文件:**新建`src/native/net/net_icmp.cpp`;修改`stdlib/net.n`、`src/native/net/net_jobs.{h,cpp}`(ICMP echo作业经submitWork提交,priority=1)、`src/native/CMakeLists.txt`、`tests/e2e/manifest.txt`;测试`stdlib_net_ping_loopback.n`、`stdlib_net_icmp_raw.n`。

**接口:**消费作业表/自管道;产出`ping`实现与IcmpSocket内部native。

- [ ] **步骤1:写失败e2e**:

`stdlib_net_ping_loopback.n`:

```nlang
import io;
import net;

int main() {
    int rtt = net.ping("127.0.0.1", 3000);
    if (rtt < 0) { return 1; }
    int rtt2 = net.ping("localhost", 3000);   //DNS path
    if (rtt2 < 0) { return 2; }
    io.print("ok");
    return 0;
}
```

`stdlib_net_icmp_raw.n`(Windows非管理员错误路径;本机非admin):

```nlang
import io;
import net;

int main() {
    try {
        IcmpSocket s = IcmpSocket.bind("127.0.0.1");
        // Administrator host: the raw socket works — smoke a send.
        s.close();
        io.print("ok");
        return 0;
    } catch (SocketException e) {
        io.print("ok");   //unprivileged: the documented error path
        return 0;
    }
}
```

- [ ] **步骤2:net.n追加**`IcmpSocket`(公开面同spec§五;`recvFrom`同R2包装):

```nlang
native int _icmpBind(string host);
native int _icmpSendTo(int handle, byte[] message, string host);
native int _icmpRecvFrom(int handle, byte[] buf, long deadlineMs);
native string _icmpLastRecvAddress(int handle);
native int _icmpClose(int handle);

class IcmpSocket {
    private int m_handle;
    public IcmpSocket(int handle) { m_handle = handle; }

    // Windows:SOCK_RAW/IPPROTO_ICMP,需管理员,否则抛 SocketException
    // ("raw ICMP requires administrator privileges on Windows")。
    // Linux:SOCK_DGRAM/IPPROTO_ICMP(免特权,受 ping_group_range 约束)。
    public static IcmpSocket bind(string host = "0.0.0.0") {
        int h = _icmpBind(host);
        if (h < 0) { _throwNet(h, "icmp bind"); }
        return IcmpSocket(h);
    }
    // 发送完整 ICMP 消息(类型/代码/校验和/ID/序列/载荷);
    // Windows raw 下校验和字段为 0 时由 native 自动计算。
    public int sendTo(byte[] icmpMessage, string host) {
        int r = _icmpSendTo(m_handle, icmpMessage, host);
        if (r < 0) { _throwNet(r, "icmp sendTo"); }
        return r;
    }
    // 收到一条 ICMP 消息,返回来源地址(out)。
    public int recvFrom(byte[] buf, out string peerAddress, int timeoutMs = 0) {
        long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0;
        int n = _icmpRecvFrom(m_handle, buf, deadline);
        if (n < 0) { _throwNet(n, "icmp recvFrom"); }
        peerAddress = _icmpLastRecvAddress(m_handle);
        return n;
    }
    public void close() {
        int r = _icmpClose(m_handle);
        if (r < 0) { _throwNet(r, "icmp close"); }
    }
}
```

(`ping`声明已在任务6落地,本任务实现:`ping(host, timeoutMs)`脚本方法化——`native int _ping(string host, long deadlineMs)`+公开`ping`包装:`long deadline = (timeoutMs > 0) ? _nowMs() + timeoutMs : 0; int rtt = _ping(host, deadline); if (rtt < 0) _throwNet(rtt, "ping " + host); return rtt;`——公开签名不变,超时经-2→TimeoutException。任务6的`native int ping(...)`声明改为脚本方法+`_ping`内部native,公开面不变。)

- [ ] **步骤3:`net_icmp.cpp`**:`_ping`=构造echo载荷→ICMP作业池(独立线程,D5)停泊于自管道(同resolve形态)→读`rttMs`(-1超时→-2,-2不可达→-4);`_icmpBind`=`NetOpenIcmp`+`NetBind`(失败文本含"administrator");`_icmpSendTo`=`NetIcmpSend`(Windows raw校验和为0时补算);`_icmpRecvFrom`=读兴趣停泊+`NetIcmpRecv`填来源。
- [ ] **步骤4:manifest两行;索引223→229(5个`_icmp*`+`_ping`);跑绿;提交**`git commit -m "feat(net): ICMP echo ping and the raw IcmpSocket"`。

---

### 任务12:并发与GC加固e2e

**文件:**新建`tests/e2e/stdlib_net_conc_echo.n`(+`.flags --carriers=4`)、`stdlib_net_parked_recv_gc.n`(+`.flags --gc-stress=4`);修改`tests/e2e/manifest.txt`。

- [ ] **步骤1:`stdlib_net_conc_echo.n`**(200连接,审查焦点5的e2e半):

```nlang
import io;
import thread;
import net;

class ConnClient {
    public int port = 0;
    public int index = 0;
    public int ok = 0;
    public void run() {
        TcpStream s = TcpStream.connect("127.0.0.1", port, 10000);
        s.write("c" + index.toString() + "\n");
        string got = s.readLine(10000);
        if (got == "c" + index.toString()) { ok = 1; }
        s.close();
    }
}

class ServeOne {
    public TcpStream conn;
    public void run() {
        string line = conn.readLine(10000);
        conn.write(line + "\n");
        conn.close();
    }
}

class Acceptor {
    public TcpListener listener;
    public void loop() {
        while (true) {
            try {
                TcpStream conn = listener.accept(2000);
                ServeOne h = ServeOne();
                h.conn = conn;
                thread.spawn(h.run);
            } catch (SocketException e) {
                return;                 //listener closed: exit
            } catch (TimeoutException e) {
                // accept轮询超时:继续等下一个连接
            }
        }
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    int port = srv.localPort();
    // Server accept loop on its own virtual thread.
    Acceptor a = Acceptor();
    a.listener = srv;
    Thread acceptor = thread.spawn(a.loop);
    Thread[] clients = Thread[200];
    ConnClient[] cs = ConnClient[200];
    for (int i = 0; i < 200; i = i + 1) {
        cs[i] = ConnClient();
        cs[i].port = port;
        cs[i].index = i;
        clients[i] = thread.spawn(cs[i].run);
    }
    int total = 0;
    for (int i = 0; i < 200; i = i + 1) { thread.join(clients[i]); total = total + cs[i].ok; }
    if (total != 200) { return 1; }
    srv.close();
    thread.join(acceptor);
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:`stdlib_net_parked_recv_gc.n`**(审查焦点4;停泊recv+分配风暴,gc-stress形态):

```nlang
import io;
import thread;
import net;

class Storm {
    public int rounds = 0;
    public void run() {
        for (int i = 0; i < 2000; i = i + 1) {
            byte[] junk = byte[64];
            junk[0] = 1;
            rounds = rounds + 1;
        }
    }
}

class Parked {
    public TcpStream s;
    public int n = -1;
    public void run() {
        byte[] buf = byte[8];
        n = s.read(buf, 10000);       //parks across the storm
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    TcpStream c = srv.accept(5000);
    Parked p = Parked(); p.s = c;
    Thread t1 = thread.spawn(p.run);
    Storm st = Storm();
    Thread t2 = thread.spawn(st.run);   //GC pressure while parked
    thread.sleep(50);
    cli.write(io.toBytes("wake"));     //client sends into the parked reader
    thread.join(t1); thread.join(t2);
    if (p.n != 4) { return 1; }
    cli.close(); c.close(); srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤3:manifest两行+`.flags`落盘;跑绿(含单载体手跑conc_echo一遍验证不依赖多载体);提交**`git commit -m "test(net): concurrency and gc-stress e2e hardening"`。

---

### 任务13:示例六件

**文件:**新建`examples/net_echo_server.n`、`net_echo_client.n`、`net_http_mini.n`、`net_udp_echo.n`、`net_ping.n`、`net_conc_bench.n`;修改`tests/e2e/examples_manifest.txt`。

**接口:**消费net包全表面;examples门=examples_manifest e2e(30s自退出,单载体)。

- [ ] **步骤1:写六件**(全部单进程自退出;风格对齐`conc_channel_pipeline.n`)。关键件全文:

`examples/net_echo_server.n`:

```nlang
// 示例:TCP echo服务器——accept循环+每连接一个虚拟线程。
// 演示:服务器惯用法、spawn状态传递(类实例字段携带连接)、EOF收尾。
import io;
import thread;
import net;

class Connection {
    public TcpStream conn;
    public int id = 0;

    public void serve() {
        int total = 0;
        while (true) {
            byte[] buf = byte[64];
            int n = conn.read(buf, 5000);
            if (n == 0) { break; }          //client EOF
            conn.write(buf, n);             //echo back
            total = total + n;
        }
        conn.close();
        io.print("connection " + id.toString() + " served "
            + total.toString() + " bytes\n");
    }
}

class AcceptLoop {
    public TcpListener listener;
    public int served = 0;

    public void run() {
        while (true) {
            try {
                served = served + 1;
                Connection c = Connection();
                c.conn = listener.accept(2000);
                c.id = served;
                thread.spawn(c.serve);
            } catch (SocketException e) {
                return;                     //listener closed: exit
            } catch (TimeoutException e) {
                // accept轮询超时:继续等下一个连接
            }
        }
    }
}

int main() {
    TcpListener listener = TcpListener.bind("127.0.0.1", 0);
    io.print("echo server on port " + listener.localPort().toString() + "\n");
    AcceptLoop loop = AcceptLoop();
    loop.listener = listener;
    Thread t = thread.spawn(loop.run);

    // Demo clients in the same process (the gate needs self-exit).
    for (int i = 0; i < 3; i = i + 1) {
        TcpStream cli = TcpStream.connect("127.0.0.1", listener.localPort(), 5000);
        cli.write("hello " + i.toString() + "\n");
        cli.shutdownWrite();                //half-close: done writing
        string reply = cli.readLine(5000);
        io.print("client " + i.toString() + " got: " + reply + "\n");
        cli.close();
    }
    thread.sleep(200);
    listener.close();
    thread.join(t);
    io.print("ok");
    return 0;
}
```

`examples/net_echo_client.n`:

```nlang
// 示例:TCP客户端——connect、文本收发、readLine分帧、优雅关闭。
import io;
import thread;
import net;

class DemoServer {
    public TcpStream conn;
    public void run() {
        string line = conn.readLine(5000);
        conn.write("echo:" + line + "\n");
        conn.close();
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    DemoServer s = DemoServer();
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    s.conn = srv.accept(5000);
    Thread t = thread.spawn(s.run);

    cli.write("morning\n");
    string reply = cli.readLine(5000);
    io.print("server says: " + reply + "\n");
    cli.close();
    thread.join(t);
    srv.close();
    io.print("ok");
    return 0;
}
```

`examples/net_http_mini.n`:

```nlang
// 示例:迷你HTTP服务器+客户端——readLine解请求行、Content-Length定长
// 体分帧(readExactly)、半关闭收尾。HTTP分层地基的可行性演示。
import io;
import thread;
import net;

class HttpConnection {
    public TcpStream conn;
    public void run() {
        string requestLine = conn.readLine(5000);
        if (requestLine == null) { conn.close(); return; }
        // 丢弃头部直到空行。
        while (true) {
            string header = conn.readLine(5000);
            if (header == null || header == "") { break; }
        }
        string path = requestLine.split(" ")[1];
        string body = "served " + path;
        conn.write("HTTP/1.0 200 OK\nContent-Length: "
            + body.length().toString() + "\n\n" + body);
        conn.shutdownWrite();
        conn.close();
    }
}

class HttpAccept {
    public TcpListener listener;
    public void run() {
        while (true) {
            try {
                HttpConnection c = HttpConnection();
                c.conn = listener.accept(2000);
                thread.spawn(c.run);
            } catch (SocketException e) {
                return;                 //listener closed: exit
            } catch (TimeoutException e) {
                // accept轮询超时:继续等下一个连接
            }
        }
    }
}

int main() {
    TcpListener listener = TcpListener.bind("127.0.0.1", 0);
    HttpAccept acc = HttpAccept(); acc.listener = listener;
    Thread t = thread.spawn(acc.run);

    TcpStream cli = TcpStream.connect("127.0.0.1", listener.localPort(), 5000);
    cli.write("GET /hello HTTP/1.0\n\n");
    string status = cli.readLine(5000);
    int contentLength = 0;
    while (true) {
        string header = cli.readLine(5000);
        if (header == null || header == "") { break; }
        if (header.startsWith("Content-Length: ")) {
            contentLength = header.substring(16, header.length()).toInt();
        }
    }
    string body = io.fromBytes(cli.readExactly(contentLength, 5000));
    io.print("status: " + status + "\n");
    io.print("body: " + body + "\n");
    cli.close();
    listener.close();
    thread.join(t);
    io.print("ok");
    return 0;
}
```

`examples/net_udp_echo.n`:

```nlang
// 示例:UDP请求/应答——sendTo/recvFrom、来源地址回发、数据报语义。
import io;
import thread;
import net;

class UdpServer {
    public UdpSocket s;
    public void run() {
        byte[] buf = byte[64];
        string from;
        int port = 0;
        int n = s.recvFrom(buf, out from, out port, 5000);
        s.sendTo(buf, from, port, n);       //只回发收到的n字节(R3)
    }
}

int main() {
    UdpSocket srv = UdpSocket.bind("127.0.0.1", 0);
    UdpServer srvTask = UdpServer(); srvTask.s = srv;
    Thread t = thread.spawn(srvTask.run);

    UdpSocket cli = UdpSocket.bind("127.0.0.1", 0);
    cli.sendTo(io.toBytes("time?"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[64];
    string from;
    int port = 0;
    int n = cli.recvFrom(buf, out from, out port, 5000);
    byte[] msg = byte[n];                   //按实收长度拷贝再转字符串
    for (int i = 0; i < n; i = i + 1) { msg[i] = buf[i]; }
    io.print("reply from " + from + ":" + port.toString()
        + " -> " + io.fromBytes(msg) + "\n");
    cli.close(); srv.close(); thread.join(t);
    io.print("ok");
    return 0;
}
```

`examples/net_ping.n`:

```nlang
// 示例:ICMP ping——往返时延、超时异常处理。
import io;
import net;

int main() {
    try {
        int rtt = net.ping("127.0.0.1", 3000);
        io.print("127.0.0.1 rtt=" + rtt.toString() + "ms\n");
        int rtt2 = net.ping("localhost", 3000);
        io.print("localhost rtt=" + rtt2.toString() + "ms\n");
    } catch (TimeoutException e) {
        io.print("no answer: " + e.message + "\n");
    } catch (SocketException e) {
        io.print("error: " + e.message + "\n");
    }
    io.print("ok");
    return 0;
}
```

`examples/net_conc_bench.n`(审查焦点5的示例半;单载体必须完成;吞吐按
ab/wrk 标准口径——并发数×总请求→msg/s+总耗时,便于与 Go/Node 同型实现
横向对比):

```nlang
// 示例:高并发证据——同进程1000虚拟线程客户端×短消息echo。
// 停泊不占载体:单载体下也必须跑完(30s门内)。吞吐口径同 wrk/ab。
import io;
import thread;
import net;

class BenchClient {
    public int port = 0;
    public int id = 0;
    public int ok = 0;
    public void run() {
        TcpStream s = TcpStream.connect("127.0.0.1", port, 20000);
        s.write("msg-" + id.toString() + "\n");
        string echo = s.readLine(20000);
        if (echo == "msg-" + id.toString()) { ok = 1; }
        s.close();
    }
}

class BenchServe {
    public TcpStream conn;
    public void run() {
        string line = conn.readLine(20000);
        if (line != null) { conn.write(line + "\n"); }
        conn.close();
    }
}

class BenchAccept {
    public TcpListener listener;
    public void run() {
        while (true) {
            try {
                BenchServe s = BenchServe();
                s.conn = listener.accept(5000);
                thread.spawn(s.run);
            } catch (SocketException e) {
                return;                 //listener closed: exit
            } catch (TimeoutException e) {
                // accept轮询超时:继续等下一个连接
            }
        }
    }
}

int main() {
    TcpListener listener = TcpListener.bind("127.0.0.1", 0);
    BenchAccept acc = BenchAccept(); acc.listener = listener;
    Thread acceptor = thread.spawn(acc.run);

    long t0 = io.nowMs();             //计时窗=并发建立+全部请求(ab/wrk口径)
    BenchClient[] cs = BenchClient[1000];
    Thread[] ts = Thread[1000];
    for (int i = 0; i < 1000; i = i + 1) {
        cs[i] = BenchClient();
        cs[i].port = listener.localPort();
        cs[i].id = i;
        ts[i] = thread.spawn(cs[i].run);
    }
    int ok = 0;
    for (int i = 0; i < 1000; i = i + 1) { thread.join(ts[i]); ok = ok + cs[i].ok; }
    long dt = io.nowMs() - t0;
    listener.close();
    thread.join(acceptor);
    io.print("1000 connections, " + ok.toString() + " echoed, "
        + dt.toString() + " ms, " + (1000000 / (dt + 1)).toString()
        + " msg/s\n");
    if (ok != 1000) { return 1; }
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:`examples_manifest.txt`追加六行**(均`name 0 ok`)。
- [ ] **步骤3:跑e2e门确认六例绿(单载体)**;提交`git commit -m "examples(net): echo, http-mini, udp, ping, and the 1000-connection bench"`。

---

### 任务14:文档(en+zh手册、CHANGELOG、VERSION、mkdocs nav)

**文件:**见文件结构表;无代码测试(由docs strict+snippet审计把关)。

- [ ] **步骤1:手册同步基线判定**(AGENTS.md:手册同步是功能任务的完成定义):`D:/dev/miniconda3/envs/py313/python.exe tools/manual_sync_check.py check`,把`docs/user_manual/SYNC_BASELINE`以来的每个提交逐个判定(语言面/标准库面/诊断文案/退出码/ABI五类);net之外的漂移也在本任务编辑中一并体现。
- [ ] **步骤2:新建`docs/user_manual/en/specification/library/20-network/`四页**(模板对齐17-concurrency-classes):
  - `index.md`:包定位(import net;停泊语义——阻塞方法让出载体,千级并发,单载体亦成立;异常家族SocketException/TimeoutException∈IOException;字节转换见io.toBytes/fromBytes(io包,r1补注5);片段审计要求:每页至少一个含main自退出片段)。必含可跑片段:

```nlang
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    if (srv.localPort() <= 0) { return 1; }   //端口0由系统分配
    srv.close();
    return 0;
}
```

  - `tcp.md`:TcpListener/TcpStream全方法表(签名+行为+抛出);自退出echo片段(任务13`net_echo_server.n`main体精简版);半关闭shutdownWrite说明;超时语义(timeoutMs=0无限等,TimeoutException)。
  - `udp.md`:UdpSocket方法表;recvFrom来源参数说明;组播/广播要点;数据报截断POSIX语义。
  - `icmp.md`:ping说明(Windows免管理员、Linux受ping_group_range);IcmpSocket管理员要求原文;片段=ping 127.0.0.1(审计真实执行)。
- [ ] **步骤3:zh镜像四页**(术语与17章zh页一致;去不必要空格)。
- [ ] **步骤4:同步既有页**:
  - `library/11-conventions.md`:包清单"六个"→"七个",加net一行;
  - `library/19-library-exceptions.md`:SocketException/TimeoutException两行;
  - `library`io页(en+zh):追加`nowMs`/`parkForIo`/`toBytes`/`fromBytes`四函数说明(停泊原语是通用IO设施,面向持fd原生库与将来管道/进程复用,普通用户无需直接使用;toBytes/fromBytes为通用UTF-8字节转换,自net包迁入,r1补注5);
  - `specification/reference/known-limitations.md`(en+zh):核对网络相关限制可否解除,登记v1实际限制(POSIX原始ICMP受`net.ipv4.ping_group_range`、IPv6测试限`::1`回环);
  - `specification/index.md`与`library`索引nav加20-network;
  - `getting-started/install-layout.md`(en+zh):stdlib文件清单加`net.n`;bin目录说明提及`nlang_net`模块(与nlang_io等原生模块同机制随包分发);
  - `mkdocs.en.yml`/`mkdocs.zh.yml` nav追加。
- [ ] **步骤5:`CHANGELOG.md`新段`## [0.13.0] - <日期>`**(Added:net包全文一段+io停泊原语一句+示例一句)+`CHANGELOG.zh-CN.md`对称段。
- [ ] **步骤6:`VERSION`→`0.13.0`**。
- [ ] **步骤7:docs strict+snippet审计跑绿**(`nlang_docs`目标+pytest经gate.sh);提交`git commit -m "docs(net): network library manual pages (en+zh), changelog, v0.13.0"`,随后`D:/dev/miniconda3/envs/py313/python.exe tools/manual_sync_check.py --update`刷新`SYNC_BASELINE`并入同一提交(AGENTS.md手册同步协议)。

---

### 任务15:全套门禁 + 10k基准 + 循环完善

**文件:**新建`tools/net_bench/net_bench_10k.n`(10000连接版net_conc_bench,不入ctest)。

- [ ] **步骤1:全量构建+ctest**:`bash tools/gate.sh build_gate temp/gate_build cmake --build build-dev --config Release`;`bash tools/gate.sh ctest_gate temp/gate_ctest ctest --test-dir build-dev -C Release`(判决行PASS;基线149+新增(test_module_state等,实施时实测))。
- [ ] **步骤2:e2e全量**:`bash tools/gate.sh e2e_gate temp/gate_e2e D:/dev/miniconda3/envs/py313/python.exe tests/e2e/run_e2e_tests.py E:/cases/nlang/dev/build-dev/src/tools/ncc/Release/ncc.exe E:/cases/nlang/dev/build-dev/src/tools/nvm/Release/nvm.exe`(基线1790+新增18条(io 1+net 17)+示例6,实测)。
- [ ] **步骤3:IDE树全量ctest**(build-ide树,含Qt族——stdlib/编译器/预置包变更硬门[[nlang-ide-tree-gate-requirement]];native模块新增亦属预置包分发面变更,核npkg含nlang_net)。
- [ ] **步骤4:Debug断言冒烟**:`bash tools/debug_assert_smoke.sh`(记忆[[nlang-assert-build-in-gates]]:Release绿≠Debug绿)。
- [ ] **步骤5:docs strict+pytest+nav-coverage**(干净树门禁配方全套选项)。
- [ ] **步骤6:10k基准**:`build-dev/src/tools/ncc/Release/ncc.exe tools/net_bench/net_bench_10k.n && build-dev/src/tools/nvm/Release/nvm.exe tools/net_bench/net_bench_10k.ncu --carriers=4`记录耗时/吞吐(msg/s,ab/wrk口径,可与Go/Node同型实现对比)/峰值内存到任务报告(spec验收2;不入ctest);另跑单载体一遍佐证"I/O并发与载体数无关"不变量。
- [ ] **步骤7:循环完善流程**:架构+代码质量双维审核(实现后双维审核记忆),Medium+发现修复后重跑受影响门禁,直到零新发现;然后按提交授权纪律收尾。

**验收判据(spec§十一逐条):**三协议e2e全绿;错误路径全绿;net_conc_bench 30s内完成(单载体);10k基准跑通记录;全套门禁GATE行全PASS;symbol_index/守卫同步;en+zh文档+示例+CHANGELOG+spec页齐备;docs审计过。

---

## 自查(spec覆盖,r1)

- spec§五API面:全部方法有归属任务(TcpListener/异常=T6,io字节转换=T5(io.toBytes/fromBytes,r1补注5),net内部_toBytes/_fromBytes=T9,TcpStream核心+accept=T7,选项=T8,分帧=T9,UDP=T10,ICMP/ping=T11);out参数经R2脚本包装保持spec公开面。
- spec§六native层:模块九TU落任务6-11;VM侧IoPoller/VmSchedulerIo=任务2/3已提交+任务6拆分;NativeHost.h v11六回调+VmWorkPool通用工作线程池=任务5(r1补注4);通用设施盘点表(r1补注5)各项落任务5(io四项/VmWorkPool)/6(moduleState)/7(submitWork消费)/9(内部字节转换);VM解耦(删intrinsic ID/BindingTable行/句柄类名/级联)=任务6;CMake/cpack=任务6/14。
- spec§七数据流:read/connect/超时竞态/close竞态=T7实现+T7/T12测试;R5/R6/R7钉死实现形态。
- spec§八测试策略:18新增net/io e2e(io 1+net 17)+2既有net e2e回归+5 ctest(2重定向+test_module_state+test_work_pool+test_net_scheduler并入)+symbol_index+docs审计+门禁=任务5-15;gc-stress双形态=T12。
- spec§九示例:六文件=T13(五例+conc_bench;单载体完成=不变量佐证)。
- spec§十集成点:1=T6-T13,2=T6-T11,3=T5/T6,4=T6-T13,5=T14,6=T14,7=T14。
- spec§十一验收:T15逐条。
- spec§十二风险:wepoll告警退路(T1 实测无/W4 /WX 门禁、警告不致败,退WSAPoll未触发,见任务1台账裁决)、ABI v11停泊解卷(T5 e2e+R7双标志,退路=regBlocking占载体+T15单载体基准佐证)、防火墙绑127.0.0.1(全局约束)、端口0(全局约束)、ICMP非admin(T11 e2e)、千并发30s(T13实测钉死)。

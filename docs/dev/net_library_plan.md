# net 网络库实施计划

> **给实施代理:** 必读子技能:用 superpowers:subagent-driven-development(推荐)或
> superpowers:executing-plans 逐任务实施本计划。步骤用复选框(`- [ ]`)跟踪。

**目标:** 按`docs/dev/net_library_design.md`实现nlang通用网络库`net`包:虚拟线程停泊
式高并发TCP/UDP/ICMP,含六个示例、en+zh手册与全套门禁。

**架构:** socket一律非阻塞;专用IO轮询线程(wepoll/epoll抽象)收割就绪事件并唤醒
`ParkedIo`停泊上下文,复刻既有定时器线程与Channel两阶段停泊握手;DNS解析与ICMP
echo在无GIL的专用工作线程上跑,经作业表回联唤醒。脚本层`stdlib/net.n`承载权威声明,
并以脚本组装`readLine`/`readExactly`/`write(string)`与带out参数的`recvFrom`包装。

**技术栈:** C++17、Winsock2 + vendored wepoll(IOCP)、POSIX骨架、CMake、nlang
intrinsic通道(通道B)、e2e(`.n`真实编译执行)+ ctest(C++)。

**Spec:** `docs/dev/net_library_design.md`(v1,已提交8b523528;本计划对§5的两处
细化见"计划级裁定")。

## 全局约束

- **前置(T1开工前)**:`git merge master`——master领先dev十二个提交(数组/张量统一wave 1、切片语法、native宿主ABI v10、手册同步基线机制bd91907c/5f023053);T12的`tools/manual_sync_check.py`与`SYNC_BASELINE`仅存在于master,先合避免收尾跨线合并([[nlang-merge-array-layout-consumers]]教训)。2026-11-06 `git merge-tree`干跑零冲突(仅known-limitations en/zh两页双方改动,可自动合并);合并后先跑build+ctest门确认绿基线再开T1。
- C++17;全部代码在`nlang::`命名空间;优先STL,不新增外部依赖(vendored wepoll除外,MIT)。
- 注释一律英文;文件头`/*--- ---*/`;source_size_guard:每文件≤500行、每函数≤50行——家族按spec§六拆TU。
- intrinsic ID只追加不复用,本批**174..220**(共47个;前置合并后master已占173=`Array.toString`,221=下一空闲);`BindingTable.cpp`键约定`pkg.name`/`Class.method`/`Class/N`;`s_HandleClassNames`只追加。
- **VM既有硬约束:intrinsic不支持out参数**(`VmExecutorOpsCalls.cpp:201`显式拒绝)——带out参数的方法必须是net.n脚本层包装(用户函数,写回合法)。
- 静态native方法走声明类的静态成员路径(计划级裁定R1,含探针);同名重载用默认参数归一(如`write(byte[] data, int count = -1)`),避免绑定键冲突。
- Windows链接`ws2_32`+`iphlpapi`(仅nlang_vm,PRIVATE);POSIX骨架v1编译通过即可;wepoll vendored到`src/3rdparty/wepoll/`并登记LICENSE。
- e2e hermetic:一律绑回环——IPv4用`127.0.0.1`,IPv6用例绑`::`连`[::1]`,组播用例绑`0.0.0.0`(join接口=默认出口接口,回环副本才投递);端口一律0(经`localPort()`读回)、30s内自退出;多载体变体用`.flags`。
- 版本0.12.6→**0.13.0**;`CHANGELOG.md`/`CHANGELOG.zh-CN.md`同步;手册en/zh成对;手册```nlang围栏含`main(`即被docs审计真实编译执行(snippets.py规则),网络片段必须自退出。
- 门禁只认`tools/gate.sh`判决行/退出码;全套含IDE树ctest、Debug断言冒烟、docs strict(见[[nlang-ide-tree-gate-requirement]]、[[nlang-cleanroom-gate-recipe]]记忆)。
- TDD:先写失败测试(e2e或ctest)再实现;真实编译`.n`+真实执行字节码。
- 提交信息英文Conventional Commits;每任务干净退出(测试全绿+零新发现)后提交。
- 构建:`cmake --build build-dev --config Release`(dev工作树的已配置树是`build-dev`,不是`build`);e2e:`D:/dev/miniconda3/envs/py313/python.exe tests/e2e/run_e2e_tests.py E:/cases/nlang/dev/build-dev/src/tools/ncc/Release/ncc.exe E:/cases/nlang/dev/build-dev/src/tools/nvm/Release/nvm.exe`(runner默认`build/`树,必须显式传build-dev二进制绝对正斜杠路径)。

## 审查焦点(Review Focus)

spec未逐一写出、但使用者最可能踩中的输入类/失效模式;每条在归属任务里钉测试:

1. **超时与数据就绪同刻竞态**:唤醒后先重试操作、再查截止——数据已到则返回,不因"已过截止"误报超时。测试:`stdlib_net_tcp_read_timeout.n`(无数据→TimeoutException)+ 同文件迟到数据变体(数据恰在截止前到→成功)。→任务5
2. **reader停泊时另一上下文close**:停泊方得`SocketException("socket closed")`而非挂死。测试:`stdlib_net_tcp_close_wakes_reader.n`。→任务5
3. **write到已关闭对端**:得`SocketException`,不挂死、不静默半写。测试:`stdlib_net_tcp_write_closed.n`。→任务5
4. **停泊期间GC风暴**:停泊帧内byte[]缓冲引用被精确根集覆盖,唤醒后缓冲完好。测试:`stdlib_net_parked_recv_gc.n`(`--gc-stress=4`)。→任务10
5. **1000停泊连接不占载体**:单载体下千并发回环echo仍须完成(停泊让出载体)。测试:`examples/net_conc_bench.n`(单载体入examples_manifest门)+ `stdlib_net_conc_echo.n`(`--carriers=4`,200连接)。→任务10/11

## 计划级裁定(对spec的细化,实施时不再重新讨论)

- **R1 静态工厂绑定路径**:首选"声明类静态native成员"路径(net.n写`public static native TcpListener bind(string host, int port);`,绑定表行`{"net","TcpListener.bind",id}`)。任务4含探针:若resolver/stamping不支持静态native(编译或绑定失败),退路=仿`Future.selectAny`在`ExprResolverBuiltinClasses.cpp`的`s_Faces`加行(每`(typeName,methodName)`恰一行,重载靠脚本包装区分),两路e2e判据相同。
- **R2 out参数包装**:`UdpSocket.recvFrom`/`IcmpSocket.recvFrom`为net.n脚本方法(out参数合法),调私有native原语`recvFromRaw`+`lastRecvAddress`/`lastRecvPort`;C++把来源存为槽内`std::string`,读取时`MintNewString`——不新增GC根。私有native若stamping不支持(探针在任务8),退路=改public并在手册标注"内部原语,配合recvFrom使用"。
- **R3 write/send计数参数**:spec§五`write(byte[] data)`增默认参`int count = -1`(全写整数组;count≥0只写前count字节);UDP的`send`/`sendTo`同样加`int count = -1`默认参(数据报同样只发前count字节)。echo服务器只回发前n字节的便利(无count参时32字节接收缓冲会全带出去);视图实参由偏移感知助手解析(任务4助手规格)。任务4同步修订spec§五行文。
- **R4 WSA生命周期落点**:不放`Runtime::StaticInit`(runtime库不链ws2_32);改`NetPlatformInit()`进程级`std::once_flag`,由net家族派发入口与轮询/工作线程首次调用。效果与spec§六"进程级一次、仅VM路径"一致,模块边界更干净。`WSACleanup`配对此once:首次init内注册`std::atexit(NetPlatformWsaCleanup)`,宿主正常退出路径执行(spec§六"WSACleanup在宿主退出路径执行";`Runtime::StaticFini`在runtime层无法调vm符号,atexit与once_flag天然配对)。
- **R5 close的fd生命周期**:按spec§七引用纪律——槽`closed`后不立即`NetClose(fd)`,待停泊者清零(唤醒方复查closed后释放)再真正关fd;无停泊者时close即关。
- **R6 轮询切片**:有新兴趣时`NetPollerWait`超时取`min(最近截止-now, kIoPollSliceMs=10ms)`,封顶新兴趣登记延迟;无兴趣时cv长睡(复刻定时器线程cv模式)。

## 文件结构

```
新建  src/3rdparty/wepoll/{wepoll.c,wepoll.h,LICENSE.txt}      vendored(IOCP epoll兼容,MIT)
新建  src/vm/net/NetPlatform.h                                  平台抽象接口
新建  src/vm/net/NetPlatformWindows.cpp                         winsock2+wepoll+IcmpSendEcho2
新建  src/vm/net/NetPlatformPosix.cpp                           POSIX骨架(编译通过)
新建  src/vm/net/NetSocketTable.h/.cpp                          每执行器socket槽表(代际防ABA)
新建  src/vm/VmSchedulerIo.cpp                                  ParkForIo/兴趣表/轮询线程
新建  src/vm/NetResolver.cpp                                    DNS线程+ICMP线程+作业表
新建  src/vm/IntrinsicsNetSupport.cpp                           家族入口/toBytes/fromBytes/扎根助手
新建  src/vm/IntrinsicsNetTcp.cpp                               TCP内在函数
新建  src/vm/IntrinsicsNetUdp.cpp                               UDP内在函数
新建  src/vm/IntrinsicsNetIcmp.cpp                              ICMP内在函数+ping
新建  stdlib/net.n                                              权威声明+脚本层方法
修改  include/nlang/vm/Intrinsics.h                             ID 174..220追加块
修改  src/compiler/builder/BindingTable.cpp                     net行+s_HandleClassNames
修改  src/vm/VmExecutorIntrinsics.cpp                           ctor链+2、net家族级联分支
修改  src/vm/VmExecutor.cpp / VmExecutor.h                      类idx缓存、异常idx、槽表/解析器成员、收尾
修改  src/vm/VmScheduler.h / VmScheduler.cpp                    Reason::Io、ParkedIo、成员、析构join
修改  src/vm/CMakeLists.txt                                     新源+wepoll+ws2_32/iphlpapi
修改  CMakeLists.txt                                            (仅当wepoll需独立目标;默认不需要)
新建  tests/test_vm/test_net_platform.cpp / test_net_slots.cpp / test_net_scheduler.cpp
修改  tests/CMakeLists.txt                                      三个ctest注册
新建  tests/e2e/stdlib_net_*.n(+.flags)                         19个e2e
新建  tests/e2e/array_size_call_expr.n                          1个e2e(编译器崩溃回归)
修改  tests/e2e/manifest.txt                                    20行
修改  tests/test_langservice/test_symbol_index.cpp              175→178、net命名空间
新建  examples/net_echo_server.n net_echo_client.n net_http_mini.n net_udp_echo.n net_ping.n net_conc_bench.n
修改  tests/e2e/examples_manifest.txt                           6行
新建  docs/user_manual/{en,zh}/specification/library/20-network/{index,tcp,udp,icmp}.md
修改  docs/user_manual/{en,zh}/specification/library/11-conventions.md 19-library-exceptions.md index.md
修改  docs/user_manual/{en,zh}/getting-started/install-layout.md mkdocs.en.yml mkdocs.zh.yml
修改  CHANGELOG.md CHANGELOG.zh-CN.md VERSION stdlib/net.n声明面 docs/user_manual/SYNC_BASELINE
新建  tools/net_bench/net_bench_10k.n                           本地基准(不入ctest)
```

## 任务依赖

```
T1平台层 → T2槽表 → T3调度器停泊 → T4包骨架(探针R1) → T5 TCP核心 → T6 TCP选项
                                                      T7 脚本分帧(依赖T5)
T5 → T8 UDP(R2探针) → T9 ICMP+ping → T10 并发/GC加固 → T11 示例 → T12 文档 → T13 全套门禁+基准
```

---

### 任务1:平台抽象层 + wepoll vendoring + CMake接线

**文件:**
- 新建:`src/3rdparty/wepoll/wepoll.c`、`wepoll.h`、`LICENSE.txt`
- 新建:`src/vm/net/NetPlatform.h`、`src/vm/net/NetPlatformWindows.cpp`、`src/vm/net/NetPlatformPosix.cpp`
- 修改:`src/vm/CMakeLists.txt`
- 测试:`tests/test_vm/test_net_platform.cpp`、`tests/CMakeLists.txt`

**接口:**
- 产出:`nlang::`自由函数`NetPlatformInit/NetOpenTcpStream/NetOpenUdp/NetOpenIcmp/NetBind/NetListen/NetConnect/NetAccept/NetRecv/NetSend/NetRecvFrom/NetSendTo/NetShutdownWrite/NetClose/NetConnectResult/NetLocalPort/NetLocalAddress/NetPeerPort/NetPeerAddress/NetSetNoDelay/NetSetKeepAlive/NetSetSendBuffer/NetSetRecvBuffer/NetSetBroadcast/NetJoinMulticast/NetLeaveMulticast/NetSetMulticastTtl/NetSetTtl/NetPollerCreate/NetPollerClose/NetPollerAdd/NetPollerMod/NetPollerDel/NetPollerWait/NetResolve/NetIcmpEcho/NetIcmpSend/NetIcmpRecv/NetLastErrorText`(任务3/5/8/9消费)。

- [ ] **步骤1:vendor wepoll**。从https://github.com/piscator/wepoll 取`wepoll.c`/`wepoll.h`(MIT),连同MIT全文存为`LICENSE.txt`(登记"Copyright (c) 2019 Oleg Kalashev")。wepoll.c自带`we_`前缀导出(`we_poll_create/we_poll_wait/...`),不与ws2_32符号冲突。

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
    #MIT — src/3rdparty/wepoll/LICENSE.txt) + winsock2 + iphlpapi
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

### 任务5:TCP核心——accept/connect/read/write/close + 解析线程

**文件:**
- 新建:`src/vm/IntrinsicsNetTcp.cpp`、`src/vm/NetResolver.cpp`(DNS作业半边)
- 修改:`stdlib/net.n`(TcpStream声明)、`src/vm/VmExecutor.h/.cpp`(解析器成员+收尾join)、`src/vm/CMakeLists.txt`、`tests/e2e/manifest.txt`
- 测试:`tests/e2e/stdlib_net_tcp_echo.n`、`stdlib_net_tcp_connect_refused.n`、`stdlib_net_tcp_read_timeout.n`、`stdlib_net_tcp_localhost.n`、`stdlib_net_tcp_close_wakes_reader.n`、`stdlib_net_tcp_concurrent_reader.n`、`stdlib_net_tcp_write_closed.n`、`stdlib_net_tcp_ipv6.n`

**接口:**
- 消费:ParkForIo、槽表、MintNetHandle、异常idx。
- 产出:`NetJobTable`+`VmExecutor::SubmitNetResolve(...)`(任务9复用作业表跑ICMP)、TcpStream全部native方法。

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
    cli.write(net.toBytes("ping"));
    //本任务只有native read/write(readExactly随任务7):4字节缓冲恰好
    //容纳回环单段echo,fromBytes(整缓冲)即整消息。
    byte[] buf = byte[4];
    int n = cli.read(buf, 5000);
    if (n != 4 || net.fromBytes(buf) != "ping") { return 1; }
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
    c1.write(net.toBytes("x"));
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
    cli.write(net.toBytes("hi"));
    byte[] buf = byte[2];
    if (c.read(buf, 5000) != 2 || net.fromBytes(buf) != "hi") { return 1; }
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
        cli.write(net.toBytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        cli.write(net.toBytes("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
        return 1;
    } catch (SocketException e) {
        io.print("ok");
        return 0;
    }
}
```

`stdlib_net_tcp_ipv6.n`(spec§三IPv6全功能:`::`双栈监听+`[::1]`括号地址,D10;测试限`::1`回环):

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
    cli.write(net.toBytes("v6"));
    byte[] buf = byte[8];
    int n = c.read(buf, 5000);
    c.write(buf, n);
    byte[] echo = byte[8];
    int m = cli.read(echo, 5000);
    if (m != 2 || net.fromBytes(echo) != "v6") { return 2; }
    cli.close();
    c.close();
    srv.close();
    io.print("ok");
    return 0;
}
```

manifest追加八行(均`... 0 ok`,connect_refused亦`0 ok`)。

- [ ] **步骤2:跑e2e确认失败**(TcpStream未声明)。
- [ ] **步骤3:`stdlib/net.n`追加TcpStream声明**(spec§五全文+R3的count默认参):

```nlang
// TCP字节流连接。connect/accept产出;read/write停泊;close释放。
class TcpStream {
    // 解析host(停泊于解析线程)并按序尝试地址;timeoutMs覆盖
    // 解析+连接全程(0=不限)。
    public static native TcpStream connect(string host, int port, int timeoutMs = 0);
    // 至多读满buf.length()字节;返回读到的字节数,0=对端已关闭(EOF)。
    public native int read(byte[] buf, int timeoutMs = 0);
    // 全写:内部循环直至count字节全部发出(默认整数组);返回发出数。
    public native int write(byte[] data, int count = -1);
    // 文本便捷面write(string)、readLine、readExactly随任务7追加
    // (脚本层方法,见任务7步骤2全文)。
    public native void close();
    // 半关闭:发出FIN,读方向保持可用。
    public native void shutdownWrite();
    public native bool isClosed();
    public native void setNoDelay(bool on);
    public native void setKeepAlive(bool on);
    public native void setSendBufferSize(int bytes);
    public native void setRecvBufferSize(int bytes);
    public native string peerAddress();
    public native int peerPort();
    public native string localAddress();
    public native int localPort();
}
```

(本任务只落native面;三个脚本方法随任务7追加,任务5的e2e不得引用它们。)

- [ ] **步骤4:`NetResolver.cpp`作业表+DNS线程**(完整设计):

```cpp
/*---
    NetResolver.cpp — the blocking-OS-call offload threads of the net
    package (net library design §6): a DNS resolver thread and an ICMP
    echo worker (separate threads: a slow getaddrinfo must not delay a
    ping, and vice versa). Jobs live in a per-executor NetJobTable;
    the context parks (ParkForIo fd=-1 deadline form) and the worker
    wakes it through VmExecutor::CompleteNetJob (takes the GIL, fills
    the record, WakeForIo). Like the timer thread, the workers are
    outside the GcWorldStop rendezvous: they never run bytecode.

    Record lifetime: the context releases its record when it resumes
    (including on timeout — the worker may still run); the worker
    marks done. The table frees a record only when both flags are set,
    so a late completion never touches freed memory.
---*/
```

- `struct NetJobRecord { int kind; bool done, released; std::vector<NetEndpoint> endpoints; bool resolveOk; int rttMs; int icmpErr; int contextId; }`+`class NetJobTable { Mint; Lookup; MarkDone(id)(GIL内由executor调); Release(id)(intrinsic侧); Submit(job); }`
- `class NetResolver { explicit NetResolver(VmExecutor&); void StartResolveJob(int ctxId, const std::string& host, int port, deadline, int jobId); void StartIcmpJob(int ctxId, const std::string& host, const uint8_t* payload, int len, deadline, int jobId); void Shutdown(); }`——两条线程惰性启动,`Shutdown()`置位+join(VmExecutor收尾调)。
- 线程体:cv等队列→取job→无GIL跑`NetResolve`/`NetIcmpEcho`→`executor.CompleteNetJob(jobId)`(GIL内:填记录、`WakeForIo(ctxId)`)。

- [ ] **步骤5:`IntrinsicsNetTcp.cpp`核心实现**(每函数≤50行;bind/accept/connect/read/write/close/shutdownWrite/isClosed/peer*/local*):

```cpp
//TcpStream.read(byte[] buf, int timeoutMs) — instance: this slot 0,
//buf slot 1, timeout slot 2. The retry loop re-enters after every
//wake (the park/resume handshake returns just past RequestPark):
//try recv first, then check closed, then the deadline (review focus
//1: data always wins over an expired bookkeeping). SetWaiter marks
//the direction before parking; EVERY exit path releases it (R5: the
//deferred fd close runs on the last release; releasing with no
//waiter set is a no-op).
bool VmExecutor::IntrinsicTcpRead(uint16_t callParamBase, uint8_t* locals,
    uint8_t* pResult)
{
    int32_t handle = 0;
    NetSocketSlot* slot = NetSlotFromReceiver(callParamBase, locals, handle);
    int32_t arrIdx = 0; size_t base = 0; int32_t cap = 0;
    ReadNetBufferArg(1, callParamBase, locals, arrIdx, base, cap);
    const int timeoutMs = ReadNetIntArg(2, callParamBase, locals);
    const auto deadline = IoDeadline(timeoutMs);   //0 -> max() sentinel
    std::vector<uint8_t> tmp(static_cast<size_t>(cap));
    for (;;) {
        const int n = NetRecv(slot->fd, tmp.data(), cap);
        if (n >= 0) {   //data or EOF (n == 0)
            m_netSlots.ReleaseWaiter(handle, NetDir::Read);
            if (n > 0)
                WriteNetBufferCells(arrIdx, base, tmp.data(), n);
            WriteNetResultInt(pResult, n);
            return true;
        }
        if (n != kNetWouldBlock) {
            m_netSlots.ReleaseWaiter(handle, NetDir::Read);
            RaiseNetSocketError("net: read failed: " + NetLastErrorText());
        }
        if (IsNetSlotClosed(handle)) {
            m_netSlots.ReleaseWaiter(handle, NetDir::Read);   //R5 drain
            RaiseNetSocketError("net: socket closed");
        }
        if (DeadlinePassed(deadline)) {
            m_netSlots.ReleaseWaiter(handle, NetDir::Read);
            RaiseNlangException(m_timeoutExcClassIdx, "net: read timed out");
        }
        if (slot->readWaiter != 0)   //one waiter per direction
            RaiseNetSocketError("net: another read is parked on this socket");
        m_netSlots.SetWaiter(handle, NetDir::Read,
                             m_spScheduler->CurrentContextId());
        ParkForCurrentIo(slot->fd, NET_EV_READ, deadline, timeoutMs > 0);
        slot = NetSlotFromReceiver(callParamBase, locals, handle); //re-lookup
    }
}
```

(`ParkForCurrentIo`=`EnsureSuspensionAllowed(); m_spScheduler->ParkForIo(selfId, fd, ev, deadline, has); RequestPark(ParkSignal::Reason::Io)`——[[noreturn]]抛出,载体搬帧,resume后从内在函数恢复点继续(IntrinsicsChannel.cpp:270-345两阶段握手模板;IO唤醒无条件,无需Channel的commit/abort复核)。`IntrinsicTcpWrite`对称:write兴趣+循环至发满;EPIPE/ERR→`RaiseNetSocketError("write failed: connection lost")`。`accept`=监听fd读兴趣+`NetAccept`+新fd入槽+`MintNetHandle(m_tcpStreamClassIdx,...)`(accept方向waiter同read纪律)。`connect`=先`SubmitNetResolve`停泊(fd=-1),再逐endpoint:`NetOpenTcpStream(ep.family)`+`NetConnect(fd,ep.address,ep.port)`→inprogress则写兴趣停泊→`NetConnectResult`;单endpoint失败即`NetClose`该fd再试下一endpoint(happy-eyeballs-lite,v6-first栈回退v4);全败→SocketException;截止跨两阶段共享(单调点)。)

其余方法契约(≤10行/个):

| id | 行为 | 抛出 |
| --- | --- | --- |
| Close | 读槽上三个waiter id→`MarkClosed`→`RemoveIoWaitsForFd(fd)`→逐个`WakeForIo`;真正关fd由最后一个`ReleaseWaiter`执行(R5);无停泊者时`MarkClosed`即关 | — |
| ShutdownWrite | `NetShutdownWrite` | SocketException |
| IsClosed | `Lookup==nullptr或closed`→1/0 | — |
| PeerAddress/PeerPort/LocalAddress/LocalPort | `NetPeer*/NetLocal*` | SocketException |

- [ ] **步骤6:`VmExecutor`收尾**:析构/Reset路径`m_netResolver.Shutdown();`+遍历`m_netSlots`关闭未关fd(spec"执行器收尾统一关闭未关socket")。
- [ ] **步骤7:`CMakeLists.txt`加`IntrinsicsNetTcp.cpp`+`NetResolver.cpp`;删Support TU临时stub的Tcp分支。**
- [ ] **步骤8:跑绿八条e2e**(单载体)+`--carriers=4`手跑echo一遍(不入manifest);全量ctest绿。
- [ ] **步骤9:提交**`git commit -m "feat(net): TCP listener/stream with parked IO and the DNS resolver thread"`。

**验收判据:**八条e2e绿;审查焦点1/2/3测试绿;全量ctest绿。

---

### 任务6:TCP选项面

**文件:**修改`src/vm/IntrinsicsNetTcp.cpp`、`tests/e2e/manifest.txt`;测试`tests/e2e/stdlib_net_tcp_options.n`。

**接口:**消费任务1的`NetSet*`;产出四个setter内在函数(声明已在任务5落地)。

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
    cli.write(net.toBytes("opt"));
    byte[] buf = byte[4];
    if (c.read(buf, 5000) != 4) { return 1; }
    cli.close(); c.close(); srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:实现四个setter**:统一助手`ApplyOption(callParamBase, locals, bool (*fn)(int,bool))`/`ApplyIntOption(..., int (*fn)(int,int))`;失败`RaiseSocketError("setsockopt <name> failed")`。
- [ ] **步骤3:manifest加`stdlib_net_tcp_options 0 ok`;跑绿;提交**`git commit -m "feat(net): TCP socket option setters"`。

---

### 任务7:脚本层分帧——readLine/readExactly/write(string)

**文件:**修改`stdlib/net.n`、`tests/e2e/manifest.txt`;测试`tests/e2e/stdlib_net_tcp_readline.n`。

**接口:**消费native `read`/`write(byte[],int)`;产出三个脚本方法(HTTP分层地基)。

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
    if (net.fromBytes(body) != "hello") { return 2; }
    cli.close();
    thread.join(t);
    srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:net.n实现**(脚本层,无新intrinsic):

```nlang
    // 文本便捷面:UTF-8编码后全写;返回发出的字节数。
    public int write(string s) {
        return this.write(net.toBytes(s));
    }

    // 读一行:以'\n'结束,返回不含行尾的文本('\r'被剥离)。
    // 对端在行结束前关闭:返回已积累文本;零字节且EOF返回null。
    public string readLine(int timeoutMs = 0) {
        string acc = "";
        byte[] one = byte[1];
        while (true) {
            int n = this.read(one, timeoutMs);
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
            acc = acc + net.fromBytes(one);
        }
    }

    // 读满count字节(HTTP定长体分帧);对端提前关闭抛SocketException。
    // read从缓冲首cell写入、无偏移入参:读入临时数组再逐cell搬运。
    public byte[] readExactly(int count, int timeoutMs = 0) {
        byte[] payload = byte[count];
        byte[] tmp = byte[count];
        int got = 0;
        while (got < count) {
            int n = this.read(tmp, timeoutMs);
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

(逐字节readLine是v1直白实现,正确优先;性能线记入文档"高频分帧建议自带缓冲"。`acc.endsWith("\r")`/`substring(start,end)`(end不含)/`got.toString()`均为现有String面(已对码核实)。)

- [ ] **步骤3:manifest加`stdlib_net_tcp_readline 0 ok`;跑绿;提交**`git commit -m "feat(net): script-layer framing — readLine, readExactly, text write"`。

---

### 任务8:UDP

**文件:**新建`src/vm/IntrinsicsNetUdp.cpp`;修改`stdlib/net.n`、`src/vm/CMakeLists.txt`、`tests/e2e/manifest.txt`;测试`stdlib_net_udp_echo.n`、`stdlib_net_udp_connected.n`、`stdlib_net_udp_multicast.n`。

**接口:**消费槽表/ParkForIo;产出UDP全部native+R2包装。

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
    cli.sendTo(net.toBytes("dgram"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[32];
    string from;
    int port = 0;
    int n = cli.recvFrom(buf, out from, out port, 5000);
    //fromBytes读整数组:先拷出恰好n字节的缓冲再比较。
    byte[] msg = byte[n];
    for (int i = 0; i < n; i = i + 1) { msg[i] = buf[i]; }
    if (n != 5 || net.fromBytes(msg) != "dgram") { return 1; }
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
    a.send(net.toBytes("rel"));
    byte[] buf = byte[3];
    if (b.recv(buf, 5000) != 3 || net.fromBytes(buf) != "rel") { return 1; }
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
    s.sendTo(net.toBytes("m"), "239.255.255.250", s.localPort());
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

- [ ] **步骤2:net.n追加UdpSocket声明**(spec§五全文;`recvFrom`为脚本包装,R2):

```nlang
class UdpSocket {
    public static native UdpSocket bind(string host = "0.0.0.0", int port = 0);
    // count默认-1=全发整数组;≥0只发前count字节(R3,回发接收缓冲用)。
    public native int sendTo(byte[] data, string host, int port, int count = -1);
    // 收一条数据报,经out参数给出来源地址/端口;停泊直至有数据或超时。
    public int recvFrom(byte[] buf, out string peerAddress, out int peerPort,
                        int timeoutMs = 0) {
        int n = this.recvFromRaw(buf, timeoutMs);
        peerAddress = this.lastRecvAddress();
        peerPort = this.lastRecvPort();
        return n;
    }
    public native void connect(string host, int port);
    public native int send(byte[] data, int count = -1);
    public native int recv(byte[] buf, int timeoutMs = 0);
    public native void setBroadcast(bool on);
    public native void joinMulticast(string group);
    public native void leaveMulticast(string group);
    public native void setMulticastTtl(int ttl);
    public native void setTtl(int ttl);
    public native string localAddress();
    public native int localPort();
    // connect后的对端(未connect时抛SocketException)。
    public native string peerAddress();
    public native int peerPort();
    public native void close();
    // 内部原语:实际收数据报并把来源存入socket槽(recvFrom包装调用)。
    private native int recvFromRaw(byte[] buf, int timeoutMs);
    private native string lastRecvAddress();
    private native int lastRecvPort();
}
```

(R2探针:私有native若编译/绑定失败→改public+手册标注"内部原语"。)

- [ ] **步骤3:`IntrinsicsNetUdp.cpp`**:bind/sendTo/connect/四个setter/local*=close契约同TCP形态(setsockopt失败抛SocketException);`recvFromRaw`=读兴趣停泊循环(同read形态)+`NetRecvFrom`填槽`lastPeerAddress/lastPeerPort`;`send`=`NetSendTo`(connected态用`NetSend`)。`recvFromRaw`完整体同任务5read骨架(换`NetRecvFrom`)。
- [ ] **步骤4:manifest三行;跑绿;提交**`git commit -m "feat(net): UDP socket with datagram parking and source reporting"`。

---

### 任务9:ICMP——IcmpSocket与ping

**文件:**新建`src/vm/IntrinsicsNetIcmp.cpp`;修改`stdlib/net.n`、`src/vm/NetResolver.cpp`(ICMP线程半边)、`src/vm/CMakeLists.txt`、`tests/e2e/manifest.txt`;测试`stdlib_net_ping_loopback.n`、`stdlib_net_icmp_raw.n`。

**接口:**消费NetJobTable;产出`INTR_Net_Ping`与IcmpSocket五原语。

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

- [ ] **步骤2:net.n追加**`ping`自由函数声明+`IcmpSocket`类(spec§五全文,`recvFrom`同R2包装:`recvFromRaw`+`lastRecvAddress`)。
- [ ] **步骤3:`IntrinsicsNetIcmp.cpp`**:`ping`=构造echo载荷→`SubmitNetIcmp`作业停泊(fd=-1截止)→唤醒读`rttMs`(-1→TimeoutException,-2→SocketException);`IcmpSocket.bind`=`NetOpenIcmp`+`NetBind`(失败文本含"administrator");`sendTo`=`NetIcmpSend`(载荷校验和Windows raw下为0时native补算);`recvFromRaw`=读兴趣停泊+`NetIcmpRecv`填来源。
- [ ] **步骤4:manifest两行;跑绿;提交**`git commit -m "feat(net): ICMP echo ping and the raw IcmpSocket"`。

---

### 任务10:并发与GC加固e2e

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
    cli.write(net.toBytes("wake"));     //client sends into the parked reader
    thread.join(t1); thread.join(t2);
    if (p.n != 4) { return 1; }
    cli.close(); c.close(); srv.close();
    io.print("ok");
    return 0;
}
```

- [ ] **步骤3:manifest两行+`.flags`落盘;跑绿(含单载体手跑conc_echo一遍验证不依赖多载体);提交**`git commit -m "test(net): concurrency and gc-stress e2e hardening"`。

---

### 任务11:示例六件

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
    string body = net.fromBytes(cli.readExactly(contentLength, 5000));
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
    cli.sendTo(net.toBytes("time?"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[64];
    string from;
    int port = 0;
    int n = cli.recvFrom(buf, out from, out port, 5000);
    byte[] msg = byte[n];                   //按实收长度拷贝再转字符串
    for (int i = 0; i < n; i = i + 1) { msg[i] = buf[i]; }
    io.print("reply from " + from + ":" + port.toString()
        + " -> " + net.fromBytes(msg) + "\n");
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

`examples/net_conc_bench.n`(审查焦点5的示例半;单载体必须完成):

```nlang
// 示例:高并发证据——同进程1000虚拟线程客户端×短消息echo。
// 停泊不占载体:单载体下也必须跑完(30s门内)。
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
    listener.close();
    thread.join(acceptor);
    io.print("1000 connections, " + ok.toString() + " echoed\n");
    if (ok != 1000) { return 1; }
    io.print("ok");
    return 0;
}
```

- [ ] **步骤2:`examples_manifest.txt`追加六行**(均`name 0 ok`)。
- [ ] **步骤3:跑e2e门确认六例绿(单载体)**;提交`git commit -m "examples(net): echo, http-mini, udp, ping, and the 1000-connection bench"`。

---

### 任务12:文档(en+zh手册、CHANGELOG、VERSION、mkdocs nav)

**文件:**见文件结构表;无代码测试(由docs strict+snippet审计把关)。

- [ ] **步骤1:手册同步基线判定**(AGENTS.md:手册同步是功能任务的完成定义):`D:/dev/miniconda3/envs/py313/python.exe tools/manual_sync_check.py check`,把`docs/user_manual/SYNC_BASELINE`以来的每个提交逐个判定(语言面/标准库面/诊断文案/退出码/ABI五类);net之外的漂移也在本任务编辑中一并体现。
- [ ] **步骤2:新建`docs/user_manual/en/specification/library/20-network/`四页**(模板对齐17-concurrency-classes):
  - `index.md`:包定位(import net;停泊语义——阻塞方法让出载体,千级并发;异常家族SocketException/TimeoutException∈IOException;toBytes/fromBytes;片段审计要求:每页至少一个含main自退出片段)。必含可跑片段:

```nlang
import net;

int main() {
    byte[] data = net.toBytes("hi");
    string back = net.fromBytes(data);
    if (back != "hi") { return 1; }
    return 0;
}
```

  - `tcp.md`:TcpListener/TcpStream全方法表(签名+行为+抛出);自退出echo片段(任务11`net_echo_server.n`main体精简版);半关闭shutdownWrite说明;超时语义(timeoutMs=0无限等,TimeoutException)。
  - `udp.md`:UdpSocket方法表;recvFrom来源参数说明;组播/广播要点;数据报截断POSIX语义。
  - `icmp.md`:ping说明(Windows免管理员、Linux受ping_group_range);IcmpSocket管理员要求原文;片段=ping 127.0.0.1(审计真实执行)。
- [ ] **步骤3:zh镜像四页**(术语与17章zh页一致;去不必要空格)。
- [ ] **步骤4:同步既有页**:
  - `library/11-conventions.md`:包清单"六个"→"七个",加net一行;
  - `library/19-library-exceptions.md`:SocketException/TimeoutException两行;
  - `specification/reference/known-limitations.md`(en+zh):核对网络相关限制可否解除,登记v1实际限制(POSIX原始ICMP受`net.ipv4.ping_group_range`、IPv6测试限`::1`回环);
  - `specification/index.md`与`library`索引nav加20-network;
  - `getting-started/install-layout.md`(en+zh):stdlib文件清单加`net.n`;
  - `mkdocs.en.yml`/`mkdocs.zh.yml` nav追加。
- [ ] **步骤5:`CHANGELOG.md`新段`## [0.13.0] - <日期>`**(Added:net包全文一段+示例一句)+`CHANGELOG.zh-CN.md`对称段。
- [ ] **步骤6:`VERSION`→`0.13.0`**。
- [ ] **步骤7:docs strict+snippet审计跑绿**(`nlang_docs`目标+pytest经gate.sh);提交`git commit -m "docs(net): network library manual pages (en+zh), changelog, v0.13.0"`,随后`D:/dev/miniconda3/envs/py313/python.exe tools/manual_sync_check.py --update`刷新`SYNC_BASELINE`并入同一提交(AGENTS.md手册同步协议)。

---

### 任务13:全套门禁 + 10k基准 + 循环完善

**文件:**新建`tools/net_bench/net_bench_10k.n`(10000连接版net_conc_bench,不入ctest)。

- [ ] **步骤1:全量构建+ctest**:`bash tools/gate.sh build_gate temp/gate_build cmake --build build-dev --config Release`;`bash tools/gate.sh ctest_gate temp/gate_ctest ctest --test-dir build-dev -C Release`(判决行PASS;基线119+新增3,合并后实测)。
- [ ] **步骤2:e2e全量**:`bash tools/gate.sh e2e_gate temp/gate_e2e D:/dev/miniconda3/envs/py313/python.exe tests/e2e/run_e2e_tests.py E:/cases/nlang/dev/build-dev/src/tools/ncc/Release/ncc.exe E:/cases/nlang/dev/build-dev/src/tools/nvm/Release/nvm.exe`(基线1755+新增20+示例6,合并后实测)。
- [ ] **步骤3:IDE树全量ctest**(build-ide树,139条(合并后重测)含Qt族——stdlib/编译器变更硬门[[nlang-ide-tree-gate-requirement]])。
- [ ] **步骤4:Debug断言冒烟**:`bash tools/debug_assert_smoke.sh`(记忆[[nlang-assert-build-in-gates]]:Release绿≠Debug绿)。
- [ ] **步骤5:docs strict+pytest+nav-coverage**(干净树门禁配方全套选项)。
- [ ] **步骤6:10k基准**:`build-dev/src/tools/ncc/Release/ncc.exe tools/net_bench/net_bench_10k.n && build-dev/src/tools/nvm/Release/nvm.exe tools/net_bench/net_bench_10k.ncu --carriers=4`记录耗时/峰值内存到任务报告(spec验收2;不入ctest)。
- [ ] **步骤7:循环完善流程**:架构+代码质量双维审核(实现后双维审核记忆),Medium+发现修复后重跑受影响门禁,直到零新发现;然后按提交授权纪律收尾。

**验收判据(spec§十一逐条):**三协议e2e全绿;错误路径全绿;net_conc_bench 30s内完成;10k基准跑通记录;全套门禁GATE行全PASS;symbol_index/守卫同步;en+zh文档+示例+CHANGELOG+spec页齐备;docs审计过。

---

## 自查(spec覆盖)

- spec§五API面:全部方法有归属任务(TcpListener/bytes=T4,TcpStream核心=T5,选项=T6,分帧=T7,UDP=T8,ICMP/ping=T9);out参数经R2包装保持spec公开面。
- spec§六native层:九个新文件全部落任务1-9;Intrinsics.h/BindingTable/s_HandleClassNames/级联=T4;CMake链接=T1;wepoll登记=T1。
- spec§七数据流:read/connect/超时竞态/close竞态=T5实现+T5/T10测试;R5/R6钉死实现形态。
- spec§八测试策略:19 net e2e+1编译器回归e2e+3 ctest+symbol_index+docs审计+门禁=任务1-13;gc-stress双形态=T10。
- spec§九示例:六文件=T11(五例+conc_bench)。
- spec§十集成点:1=T4-T11,2=T1-T9,3=T1-T10,4=T12,5=T12,6=T12。
- spec§十一验收:T13逐条。
- spec§十二风险:wepoll告警退路(T1 /W3+退WSAPoll)、防火墙绑127.0.0.1(全局约束)、端口0(全局约束)、ICMP非admin(T9 e2e)、千并发30s(T11实测钉死)。

/*---
    VmScheduler.h — 上下文调度器（并发 K1 决策 5，2026-10-19；K2 多载体
    化 2026-10-20）

    所有上下文（main 与后续 spawned 虚拟线程/协程）由载体执行：FIFO
    运行队列逐上下文出队；回边配额（DEFAULT_SCHEDULER_TRIP_BACKEDGES）
    到期且另有 Runnable 上下文时经 ParkSignal(Preempted) 让渡（D6：
    仅以队列为工作信号，GIL 等待计数不参与让渡决策）。K2 起载体可有
    多个（主载体＝宿主线程＋辅助线程池），全部经同一把 GIL 串行化——
    「取队与执行同处一个持有窗口」＋FIFO ⇒ 上下文的执行窗口序与载体数
    无关（INV-3）。

    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 sleep 定时器 +
    Slice 8 互斥停泊/唤醒 + Slice 9 通道停泊/唤醒 + Slice 10 载体在飞
    标志 + K2：条件变量谓词等待（空队列时载体让出 GIL 等待，替代 K1 的
    sleep_until）、死锁检测（全部载体将等待且无唤醒源即抛）、每载体
    状态（当前上下文/嵌套深度/配额预算——多个载体并发在飞，单成员会
    错误串行化）、定时器专用懒线程（到期迁移集中在该线程，载体对运行
    队列纯谓词等待，无截止时间簿记）。

    帧的存放约定（「停泊即窃取」）：运行中上下文的帧在其载体线程的
    求值栈上（CarrierState::evalStack，K7 私有化；宿主线程经回落槽
    同形态）；挂起时整栈 move 进 ExecutionContext::frames，恢复时
    move 回——每线程一个求值栈，GC 根集、异常走查、调试视图面向
    「当前线程栈＋注册表停泊帧」。
---*/
#pragma once
#include "CallFrame.h"
#include "GcWorldStop.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace nlang {

//回边配额（决策 5）：每 N 次回边安全点允许一次调度决策。
constexpr int DEFAULT_SCHEDULER_TRIP_BACKEDGES = 1000;

//K4 唤醒亲和（D7）：出队偏好扫描只看队首常数窗——窗口内找得到自己
//跑过的上下文就取它，否则回退队首；O(1) 摊还（每载体每次出队至多
//多扫 8 项），每次出队恰消费一项故无饥饿。
constexpr size_t AFFINITY_SCAN_WINDOW = 8;

//K7: GIL 持有者接口与线程局部属主标记——守卫家族随锁的拥有者（调度
//器）定居，调度器公共面据此自守卫（嵌套零开销：本线程已持有则标记
//已设，内层守卫不动锁）。
//
//K8 语义改述：这把锁是**调度器域锁**——守调度器状态（运行队列、上
//下文注册表、调试焦点、定时器表）与载体的分派持有窗；执行器存储已
//迁入五域锁（VmDomainLocks.h）。它位于域锁层级之外：域锁永不跨调
//度器调用持有（无 ABBA 的不变量）；分派窗是唯一被认可的「持调度器
//锁再向内取域锁」形态（它是载体一切执行的根）。
//
//标记不变量：进程内对 m_gil 的一切获取都经某个会设置 t_gilOwner 的
//持有者（窗口守卫、ensure 守卫、定时器循环顶、调度器自守卫方法）。
//绕开持有者裸取锁会使该线程下一次嵌套 ensure 守卫在非递归互斥量上
//二次加锁＝自死锁。任何路径都不得无持有者锁定 m_gil。
class GilHolder {
public:
    virtual ~GilHolder() = default;
    virtual void Release() = 0;   //unlock + clear owner
    virtual void Acquire() = 0;   //counted lock + set owner
};

//本线程当前的 GIL 持有者（null = 未持有）。属主身份是线程态不是锁
//态：条件变量等待期间锁暂离而属主标记保持（其间只跑 VM 内部代码，
//无人观察间隙）；阻塞原生臂经它交接锁。
inline thread_local GilHolder* t_gilOwner = nullptr;

//挂起/抢占信号（决策 2 F1，零新操作码）。刻意不继承 std::exception：
//它不是错误，任何按 std::exception 捕获的位点不得拦截；RunFrames 的
//兜底 catch(...) 之前置有 ParkSignal 直通分支予以让路。
struct ParkSignal {
    enum class Reason { Preempted, Timer, Join, Mutex, ChanRecv, ChanSend };
    Reason reason;
    explicit ParkSignal(Reason r) : reason(r) {}
};

//K2 slice 4（§2.5）：关停穿透信号。载体在阻塞原生中期间 run 结束（main
//返回/失败或致命错误）——原生返回后发现 stop 已置位即抛本信号：不是错
//误，与 ParkSignal 同样的直通待遇；载体循环捕获后静默退出（帧已整栈
//停泊在 ctx，K1 退出策略原样丢弃）。
struct ShutdownSignal {};

//RAII：嵌套求值窗口（toString / 宿主 CallByName）进行期间递增本载体
//的嵌套深度。窗口内的指令中途回不来，停泊必须整帧栈让渡，因此窗口
//内禁止一切挂起（配额直接跳过；RequestPark 将内在函数的挂起请求转为
//可捕获的基础异常）。
class NestEvalGuard {
public:
    explicit NestEvalGuard(int& depth) : m_depth(depth) { ++m_depth; }
    ~NestEvalGuard() { --m_depth; }
    NestEvalGuard(const NestEvalGuard&) = delete;
    NestEvalGuard& operator=(const NestEvalGuard&) = delete;
private:
    int& m_depth;
};

//RAII：载体在飞计数（Slice 10 的 bool 泛化到多载体）。CarrierLoop 进入
//递增、任何退出路径（含异常）递减。thread.spawn 以此区分两种嵌套窗
//口：宿主回调（脚本运行期间经宿主函数表再入——载体在飞，spawn 合
//法，返回后续由载体调度）与嵌入宿主 call()（无载体轮转——新上下文
//永无调度者，当场拒绝而非静默铸出永不执行的上下文）。原子量：递增
//发生在取 GIL 之前，读取发生在窗口内，须跨线程可见。
class CarrierActiveGuard {
public:
    explicit CarrierActiveGuard(std::atomic<int>& count) : m_count(count) {
        m_count.fetch_add(1, std::memory_order_acq_rel);
    }
    ~CarrierActiveGuard() {
        m_count.fetch_sub(1, std::memory_order_acq_rel);
    }
    CarrierActiveGuard(const CarrierActiveGuard&) = delete;
    CarrierActiveGuard& operator=(const CarrierActiveGuard&) = delete;
private:
    std::atomic<int>& m_count;
};

enum class ContextState {
    Runnable,        //在运行队列中，或正被载体执行
    ParkedTimer,     //Slice 7：sleep 停泊，wakeAt 到期唤醒
    ParkedJoin,      //Slice 6：join 目标未完成停泊
    ParkedMutex,     //Slice 8：锁竞争停泊
    ParkedChanRecv,  //Slice 9：空通道接收停泊
    ParkedChanSend,  //K3 task 4：容量满的通道发送停泊
    BlockedNative,   //K2 slice 4：载体在阻塞原生中（帧已窃入 ctx、
                     //GIL 已释放；原生返回后载体原地续跑，不入队）
    Finished,        //根帧正常返回
    Failed,          //根帧异常逃逸
};

//一个调度上下文（main 或后续 spawned 线程/协程）。frames 见文件头
//「停泊即窃取」约定；其余字段由各生产者切片填充，本片恒零值。
struct ExecutionContext {
    int id = 0;
    ContextState state = ContextState::Runnable;
    std::vector<CallFrame> frames;
    //K7：停泊完成握手——park 位点已迁移状态（ParkedJoin 等）但
    //ParkSignal 仍在展开、帧尚未从载体求值栈搬入 frames 的窗口内为
    //false。唤醒路径只入队已完成停泊者；未完成者的入队由停泊载体在
    //搬帧的同一临界区内补做（帧先落位、队列项后出现——出队者恒见帧
    //已就位，消除「无帧弹出＝全新入口重跑」竞争）。恢复取回帧时清零。
    bool framesParked = false;
    int32_t threadObjHeapIdx = 0;   //Slice 6：句柄对象（K1 Thread / K3
                                    //Future，GC 根 + join 入口）
    int32_t failExcHeapIdx = 0;     //Slice 6：失败重抛的异常对象
    //Slice 7：ParkedTimer 的唤醒时刻（epoch = 未排定）。
    std::chrono::steady_clock::time_point wakeAt{};
    //Slice 8/9：停泊目标（互斥体/通道）的句柄；0 = 无。
    int32_t parkHandle = 0;
    bool rethrowOnResume = false;   //Slice 6：join 失败重抛标记
    int64_t resultCell = 0;         //根帧返回值（main 退出码 / Future 取值）
    //K3：根帧返回种类（RTK_*）。根帧 Finished 后 frames 已空、
    //resultCell 不再被任何帧根覆盖，GC 的调度器根臂据本字段判定
    //resultCell 是否持引用域值（MarkFrameResult 同款判别）。
    uint8_t resultKind = 0;
    //K3：get() 停泊者的取值目的地（调用点的 pResult 槽，指向停泊帧
    //localsBuf 内部——停泊期间帧整体驻留注册表，缓冲地址稳定）。
    //目标 Finished 时由 DeliverJoinResults 写入并清空；join 停泊者
    //恒为 null。失败唤醒时随 rethrowOnResume 一并清空（帧即将 unwind）。
    uint8_t* joinResultDst = nullptr;
    //K3：恢复时重抛的异常对象（通道 close 唤醒已排干 recv 等待者的臂
    //——对象在 close 调用内预建并经本字段扎根，恢复时
    //ArmResumeRethrow 直取；join 失败重抛不走此字段，仍按 parkHandle
    //取目标 failExcHeapIdx）。
    int32_t resumeExcHeapIdx = 0;
    //K4：最近一次分派本上下文的载体编号（255 = 从未分派）。分派时盖
    //戳；出队偏好扫描把它当「匹配自己」消费，255（无偏好）匹配任意
    //扫描者——单载体下一切项都命中，最早命中恒为队首，严格退化为纯
    //FIFO。
    uint8_t lastCarrier = 255;
};

//调度器本体（VmExecutor 经 unique_ptr 持有）。注册（main/spawned）、
//FIFO 出队、停泊/唤醒迁移与观测。帧的搬运（载体求值栈 ⇄ ctx->frames）
//由执行器完成——调度器只管状态与队列。
class VmScheduler {
public:
    static constexpr int kMainThreadId = 0;

    //K7: 可重入 GIL ensure 守卫——本线程未持有则获取（嵌套使用＝空操
    //作，见 t_gilOwner 的标记不变量）。调度器公共变更面经它自守卫
    //（拥有者强制锁纪律，调用点无须记忆）；定时器线程循环顶的获取
    //也走它——条件变量等待经 Lock() 释放/重取锁（属主是线程标记而
    //非锁态：等待期间锁暂离、标记保持，访问器由此成为承重件）。
    class GilEnsureGuard : public GilHolder {
    public:
        explicit GilEnsureGuard(VmScheduler& sched) : m_rSched(sched) {
            if (t_gilOwner != nullptr) {
                //already held on this thread — nested no-op (marker hit)
                m_rSched.m_storeGuardMarkerHits.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            m_rSched.m_storeGuardAcquires.fetch_add(
                1, std::memory_order_relaxed);
            Acquire();
        }
        GilEnsureGuard(const GilEnsureGuard&) = delete;
        GilEnsureGuard& operator=(const GilEnsureGuard&) = delete;
        ~GilEnsureGuard() {
            if (m_lock.owns_lock())
                Release();
        }
        //条件变量等待在 cv 内部释放并重取锁——把 unique_lock 交出去
        //给 wait 调用；等待期间属主标记保持（见 t_gilOwner 契约）。
        std::unique_lock<std::mutex>& Lock() { return m_lock; }
        void Release() override {
            m_lock.unlock();
            if (t_gilOwner == this)
                t_gilOwner = nullptr;
        }
        void Acquire() override {
            const auto waitStartedAt = m_rSched.BeginGilWait();
            m_lock.lock();
            m_rSched.EndGilWait(waitStartedAt);
            t_gilOwner = this;
        }
    private:
        //Declaration order matters: m_lock's initializer reads m_rSched.
        VmScheduler& m_rSched;
        std::unique_lock<std::mutex> m_lock{
            m_rSched.m_gil, std::defer_lock};
    };

    //K7 自守卫纪律：公共变更面（注册/复位/出队/停泊/唤醒/终结/簿记/
    //焦点/定时器服务）一律经 GilEnsureGuard 自守卫——嵌套调用零开销，
    //未持锁调用真实获取；私有助手假设调用方已持锁。例外保留调用方契
    //约的是白盒观测方法（ContextCount/HeldContextOfCarrier/
    //HoldingCarrierOf/DebugFocusContext 等——持锁或静息期调用）。
    //
    //每轮执行前复位（VmExecutor::ResetPerRunState 调用，决策 5 显式
    //枚举清空）：上下文注册表、运行队列、定时器表与关停标志全部清空
    //——嵌入宿主复用同一执行器的前提。在飞载体计数不清零：由载体循
    //环自身的 RAII 归零，此处重置反而会掩盖池未汇合的泄漏。
    void Reset();

    //注册新上下文并入队尾；id 单调分配（main 恒为 0）。自守卫。
    int RegisterContext();

    //FIFO 弹出下一个待运行上下文并记为本载体当前上下文。K7：契约翻
    //转——内部自守卫（焦点门谓词等待与空队列 cv 等待经守卫的 Lock()
    //释放/重取锁，属主标记保持），**返回时不持锁**——出队后载体无锁
    //进入执行段，会合参与（BeginExecuting）无须任何锁迁移舞步。
    //K4 调试焦点门（D2）：焦点上下文 Runnable 时它是唯一可分派者——
    //排队中则本次出队即取焦点（越过队首非焦点项）；被某载体的执行窗
    //口持有时无物可分派，在条件变量上等焦点释放（停泊/终结/重新入队
    //都会通知）；焦点停泊/终结则无状态放行（每次出队按当前状态重判，
    //无清除路径）。队列空时在条件变量上纯谓词等待（无截止时间——定时
    //器的到期迁移由专用线程负责，到期入队即唤醒载体）。关停请求置位
    //返回 -1。死锁检测：全部在飞载体都将等待（本载体即将成为最后一个
    //空闲载体）且队列与定时器表皆空——没有任何唤醒源，直接抛错（main
    //未完成即无法返回，与其静默挂死不如拒绝）；定时器表非空则必有未来
    //唤醒，不构成死锁。焦点被持等待臂不做死锁检测：焦点 Runnable 即
    //存在工作，持有者的窗口必然结束并通知。
    int PopNextRunnable();

    //K7 site 1: 停泊过渡——把调用线程整条求值栈搬入上下文的停泊帧
    //（载体停泊臂，GIL-free 调用点——这是唯一的锁外帧迁移）。搬移经
    //拥有者方法在全局锁下串行：并发收集者恰好扫「注册表条目」与
    //ctx->frames」两个面，任一时刻每帧恰在唯一可见面。自守卫。
    //K7：同一临界区内决定队列项——状态仍 Runnable（配额抢占，或停泊
    //展开途中已被唤醒）则搬帧后入队；停泊中则置 framesParked 交由唤
    //醒路径入队（握手见 ExecutionContext::framesParked）。
    void ParkContextFrames(int id, std::vector<CallFrame> frames);

    //队列中仍有待运行上下文（配额抢占的门槛：载体当前上下文已出队，
    //队列成员全部是「另外的」上下文）。自守卫读取——回边配额分支在
    //不持锁下调用；自锁读取者故非 const。
    bool HasOtherRunnable() {
        GilEnsureGuard g(*this);
        return !m_runQueue.empty();
    }

    //抢占后重新入队尾（载体不再持有该上下文）。K7 起抢占重入并入
    //ParkContextFrames 的同一临界区——生产路径不再调用，保留给白盒
    //测试直驱。
    void Requeue(int id);

    //join 停泊（Slice 6）：state → ParkedJoin，parkHandle 记目标；帧的
    //搬运由载体统一完成。目标未在运行队列之列——唤醒只经 WakeJoiners。
    void ParkForJoin(int id, int targetId);

    //sleep 停泊（Slice 7）：state → ParkedTimer、记唤醒时刻并入定时器
    //表；不入运行队列——到期由 PopNextRunnable 的空队列处理分支迁回 Runnable。
    void ParkForTimer(int id,
                      std::chrono::steady_clock::time_point wakeAt);

    //互斥停泊（Slice 8）：state → ParkedMutex。等待者集合在执行器的
    //MutexSlot 侧表（FIFO）——调度器不记 parkHandle，唤醒只经
    //WakeForMutex（unlock 的所有权移交触发）。
    void ParkForMutex(int id);
    //互斥唤醒（Slice 8）：unlock 把所有权移交给队首等待者后调用——回
    //Runnable 入队尾。
    void WakeForMutex(int id);

    //通道接收停泊（Slice 9）：state → ParkedChanRecv。等待者集合在执
    //行器的 ChannelSlot 侧表（FIFO，各携带唤醒时写值的目标指针）——
    //调度器不记 parkHandle，唤醒只经 WakeForChanRecv（send 的直接交付
    //或 close 的全员唤醒触发）。
    void ParkForChanRecv(int id);
    //通道唤醒（Slice 9）：回 Runnable 入队尾（值已由执行器在调用本方
    //法前写入停泊帧的结果槽）。
    void WakeForChanRecv(int id);

    //K3 task 4：有界通道发送停泊——待发值与属主队列（FIFO）在执行器的
    //ChannelSlot 侧表，调度器只做状态迁移（同 ParkForChanRecv 形态）。
    void ParkForChanSend(int id);
    //K3 task 4：唤醒停泊发送者——recv 已把队首待发值挪入缓冲（仅续跑，
    //停泊的调用不会重执行），或 close 已在其上预置关闭异常重抛。
    void WakeForChanSend(int id);

    //目标上下文终结（Finished/Failed）时唤醒其全部 joiner：回 Runnable
    //；已完成停泊者入队尾（停泊展开途中被唤醒者由停泊载体补推，握手
    //见 ExecutionContext::framesParked）；failed=true 时置
    //rethrowOnResume（joiner 恢复时重抛目标的 failExcHeapIdx）。多
    //joiner 皆唤醒（计划决策 6）。
    void WakeJoiners(int targetId, bool failed);

    //K3：把 Finished 目标的取值投递给全部 get() 停泊者（joinResultDst
    //非 null、parkHandle 命中本目标的 ParkedJoin 上下文）——停泊的调用
    //恢复时不重执行，值须在唤醒前写入（recv 同款唤醒时交付契约）。
    //在 WakeJoiners 之前调用：先填值后入队，唤醒者恢复即见已填槽。
    void DeliverJoinResults(int targetId, int64_t resultCell);

    //K2 slice 4（§2.5）：阻塞原生停泊/复原对。MarkBlockedNative 在窃帧
    //后、释放 GIL 前调用（帧已入 ctx、载体仍持有该上下文——
    //currentContextId 不清零）；MarkRunnableHeld 在重取 GIL、复原帧后调
    //用（状态回 Runnable 但不入队——载体持有语义，同运行中上下文）。
    //计数是死锁检测的排除项：阻塞原生完成即唤醒源，全部载体等待＋队
    //列/定时器皆空时若仍有阻塞节在飞，不得判死锁。自守卫。
    void MarkBlockedNative(int id);
    void MarkRunnableHeld(int id);
    //stop 标志读取（须持 GIL——阻塞臂在重取 GIL 后检查）。main 终结或
    //运行失败后置位；载体循环以此区分 run 已结束。
    bool StopRequested() const { return m_stopRequested; }

    void MarkFinished(int id, int64_t result);
    void MarkFailed(int id, int32_t failExcHeapIdx);

    //观测（白盒测试注入断言）。ContextCount 等读取者保持调用方契约
    //（持锁或静息期）；Find/FindMutable 是热路径查表，自守卫（自锁读
    //取者故非 const——经 unique_ptr 访问不受执行器侧 const 传染）。
    size_t ContextCount() const { return m_contexts.size(); }
    const ExecutionContext* Find(int id);
    //本载体正在执行的上下文（-1 = 空闲）。多载体并发在飞，「当前」
    //是每载体概念：读安装在本线程上的载体槽；未安装（宿主线程在载
    //体循环之外）返回 -1。
    int CurrentContextId() const {
        return t_carrier ? t_carrier->currentContextId : -1;
    }
    //在飞载体计数（spawn 门判定：零即在载体轮转之外）。
    int ActiveCarrierCount() const {
        return m_activeCarrierCount.load(std::memory_order_acquire);
    }
    //CarrierActiveGuard 的递增/递减目标。
    std::atomic<int>& ActiveCarrierCountRef() {
        return m_activeCarrierCount;
    }
    //正在竞争 GIL 的载体数。K2 D6：纯白盒观测（多载体真实竞争的证
    //据），不再是让渡规则的输入——一切唤醒路径先入队再通知，空队列
    //的锁等待者必无工作，按计数让渡会自激振荡（convoy 效应，见
    //VmExecutorOpsControl.cpp 的 CheckSchedulerQuota 注释）。
    int GilWaiters() const {
        return m_gilWaiters.load(std::memory_order_acquire);
    }

    //K6 D9: run generation — bumped by Reset(). A carrier snapshots it
    //at install (pool launch, once per run); a mismatch after a blocking
    //native reacquires the GIL means the run was torn down underneath
    //(a zombie past the join grace) and the thread must exit without
    //touching the new run's state. K7: the member is atomic — the
    //safepoint poll now reads it GIL-free (backedges) to eject zombie
    //carriers mid-bytecode; relaxed ordering suffices (the GIL carries
    //the semantics, the load only needs tear-freedom).
    uint64_t RunGeneration() const {
        return m_runGeneration.load(std::memory_order_relaxed);
    }
    uint64_t CarrierRunGeneration() {
        return carrierSlot().runGeneration;
    }
    //K7: zombie discriminator for INSTALLED carrier slots — true when
    //Reset advanced the generation past this thread's install snapshot.
    //Poll sites (backedge/function-entry safepoints) call this GIL-free
    //and throw ShutdownSignal on staleness: K6 excluded mid-bytecode
    //zombies by holding the GIL across every window, so a run could not
    //tear down under a blocking-native callback mid-execution; the
    //GIL-free dispatch window reopens that shape, and without this
    //guard a zombie callback would run forever outside every rendezvous
    //denominator. Host threads (no installed slot) never read stale —
    //their calls are bounded by the embedder's own synchronization
    //(embedding contract, unchanged from K6).
    bool CarrierRunGenerationStale() const {
        return t_carrier != nullptr && t_carrier->carrierId >= 0
            && t_carrier->runGeneration
                != m_runGeneration.load(std::memory_order_relaxed);
    }

    //K6: stop-the-world rendezvous handle — the safepoint protocol
    //object lives with the scheduler so every run (and every carrier)
    //shares one instance.
    GcWorldStop& GcStop() { return m_gcStop; }

    //K2 slice 2: 每载体状态。载体循环入口（主/辅助同一点）安装本线程
    //的槽；InstallSelfCarrier 幂等重置——同一 OS 线程跑过多个执行器
    //或多次 run 时不携带旧值。槽按线程隔离，多个载体并发在飞时各自
    //读写互不可见。K4：载体编号（主=0、辅助=1..N）随槽安装——集中登记
    //表与唤醒亲和都以它为键。
    struct CarrierState {
        int currentContextId = -1;  //本载体正在执行的上下文
        int carrierId = -1;         //K4：本载体编号（主=0、辅助=1..N）
        int nestDepth = 0;          //本载体上的嵌套求值深度
        //本载体当前窗口的回边预算。仅属主载体线程读写（分派时重装、
        //回边递减——K7 核证：他线程永不触及他人槽），故无须 GIL——
        //该前提被破坏时须先改为原子或加锁。
        int schedTripBudget = 0;
        uint64_t runGeneration = 0; //K6 D9: run generation at install
                                     //(zombie guard — compare after a
                                     //blocking native)
        //K7 frame privatization: the evaluation stack lives on the slot —
        //running frames belong to the thread executing them. Host threads
        //re-entering through the bridge use the fallback slot's stack.
        //Companion state (unwind records, resume-rethrow arming) is
        //per-thread for the same reason it was executor-global before:
        //only one window runs at a time per thread.
        std::vector<CallFrame> evalStack;
        std::vector<UnwindFrame> unwindFrames;
        int32_t resumeThrowExcIdx = 0;  //0 = none; see VmExecutor.h
        //K7 site 4: the participation-root depth for the GC poll gate
        //(CheckGCSafepoint polls at nestDepth <= this value). 0 for
        //carriers (poll at the dispatch root); a bridge RAII that flips
        //this thread's executing bit on records depth+1 here for its
        //duration, so host-outermost runs and blocking-native callbacks
        //keep polling inside their windows.
        int gcPollRootDepth = 0;
    };
    void InstallSelfCarrier(int carrierId) {
        t_carrier = &t_selfCarrier;
        t_carrier->currentContextId = -1;
        t_carrier->carrierId = carrierId;
        t_carrier->nestDepth = 0;
        t_carrier->schedTripBudget = 0;
        t_carrier->runGeneration =
            m_runGeneration.load(std::memory_order_relaxed);
        t_carrier->evalStack.clear();
        t_carrier->unwindFrames.clear();
        t_carrier->resumeThrowExcIdx = 0;
        t_carrier->gcPollRootDepth = 0;
    }
    //载体槽访问（嵌套深度/预算/求值栈/伴随态）。未安装线程（理论上
    //只有宿主在轮转外直调工具函数）落到线程局部空槽——恒零值，行为
    //等同 K1。
    int& CarrierNestDepthRef() { return carrierSlot().nestDepth; }
    //K8: the calling thread's carrier id (-1 = host fallback slot).
    int CurrentCarrierId() { return carrierSlot().carrierId; }
    int& CarrierTripBudgetRef() { return carrierSlot().schedTripBudget; }
    //Non-const only, deliberately: a const overload would need its own
    //fallback-slot instance (thread_local statics in const/non-const
    //carrierSlot() are distinct objects) and route reads to a different
    //slot than writes.
    std::vector<CallFrame>& CarrierEvalStackRef() { return carrierSlot().evalStack; }
    std::vector<UnwindFrame>& CarrierUnwindFramesRef() { return carrierSlot().unwindFrames; }
    int32_t& CarrierResumeThrowExcIdxRef() { return carrierSlot().resumeThrowExcIdx; }
    int& CarrierGcPollRootDepthRef() { return carrierSlot().gcPollRootDepth; }
    void ResetCarrierTripBudget(int limit) {
        carrierSlot().schedTripBudget = limit;
    }

    //GIL wait accounting (maintained by the acquire brackets in the
    //executor's lock holders — GilEnsureGuard's real acquisitions, the
    //store guards' acquisitions, freeze-park reacquisitions — and the
    //timer thread's loop-top lock); the peak is the white-box contention
    //evidence. K5 batch 1 adds the time dimension: cumulative wait
    //nanoseconds and the contended-acquire count (another waiter already
    //queued when this one began — convoy formation; a lone waiter
    //contending only with the holder does not count). Lock
    //reacquisitions inside condition-variable waits stay excluded (same
    //ruling as ServiceDueTimers above).
    std::chrono::steady_clock::time_point BeginGilWait() {
        const auto startedAt = std::chrono::steady_clock::now();
        if (m_gilWaiters.load(std::memory_order_acquire) > 0)
            m_gilContendedAcquires.fetch_add(1, std::memory_order_relaxed);
        int cur = m_gilWaitersPeak.load(std::memory_order_relaxed);
        const int now = m_gilWaiters.fetch_add(1, std::memory_order_acq_rel)
                        + 1;
        while (now > cur
               && !m_gilWaitersPeak.compare_exchange_weak(
                      cur, now, std::memory_order_acq_rel))
            {}
        return startedAt;
    }
    void EndGilWait(std::chrono::steady_clock::time_point startedAt) {
        const auto endedAt = std::chrono::steady_clock::now();
        m_gilWaiters.fetch_sub(1, std::memory_order_acq_rel);
        m_gilWaitNanosTotal.fetch_add(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    endedAt - startedAt).count()),
            std::memory_order_relaxed);
    }
    int GilWaitersPeak() const {
        return m_gilWaitersPeak.load(std::memory_order_acquire);
    }
    //K5 batch 1 white-box observability: cumulative GIL wait time and
    //contended-acquire count (per run; cleared by Reset).
    uint64_t GilWaitNanosTotal() const {
        return m_gilWaitNanosTotal.load(std::memory_order_acquire);
    }
    uint64_t GilContendedAcquires() const {
        return m_gilContendedAcquires.load(std::memory_order_acquire);
    }
    //K7 store-guard telemetry (per run): GilEnsureGuard classifies each
    //construction — a real mutex acquisition, or a marker hit (the
    //guard found this thread already owns the global lock and took
    //nothing; the nested fast path). The ratio shows how much re-entrant
    //guard nesting avoids the mutex. Cleared by Reset.
    uint64_t StoreGuardAcquisitions() const {
        return m_storeGuardAcquires.load(std::memory_order_acquire);
    }
    uint64_t StoreGuardMarkerHits() const {
        return m_storeGuardMarkerHits.load(std::memory_order_acquire);
    }
    //辅助载体上的分派次数（白盒观测：多载体并非空转的证据）。主载体
    //线程在 RegisterContext（主载体专属路径）时记录。
    int AuxDispatchCount() const { return m_auxDispatchCount; }

    //K4：亲和观察计数（白盒）。命中＝出队取到的项恰盖本载体戳（亲和
    //真实生效）；未命中＝取到无偏好项（255）或窗口耗尽回退队首。
    size_t AffinityHits() const { return m_affinityHits; }
    size_t AffinityMisses() const { return m_affinityMisses; }

    //K4: register the carrier count and size the central registry (the
    //primary-carrier path RunPrimaryCarrier calls this before the pool
    //launches — a single-threaded moment, so no concurrent resizing;
    //reaffirmed every run). The mirror table is indexed by carrier id
    //and holds "the context this carrier currently holds" (-1 = idle);
    //it feeds the debugger's threads listing (held markers) and the
    //affinity counters. Frame reachability does NOT depend on it (the
    //inspection surface is complete on two paths: an execution-window
    //context is held by its carrier (frames on its evaluation stack),
    //everything else reads the registry's parked frames).
    void ConfigureCarriers(uint32_t count) {
        GilEnsureGuard g(*this);
        m_carrierCount = count;
        m_heldByCarrier.assign(count, -1);
    }
    //K4: the context a carrier currently holds (white-box observation;
    //an out-of-range carrier id returns -1).
    int HeldContextOfCarrier(int carrierId) const {
        if (carrierId < 0
            || static_cast<size_t>(carrierId) >= m_heldByCarrier.size())
            return -1;
        return m_heldByCarrier[static_cast<size_t>(carrierId)];
    }
    //K4: reverse lookup — which carrier's execution window holds this
    //context (-1 = none: parked/queued/finished). Must hold the GIL
    //(reads the central registry). The carrier count is on the pool's
    //maximum scale, so a linear scan suffices.
    int HoldingCarrierOf(int contextId) const {
        for (size_t i = 0; i < m_heldByCarrier.size(); ++i)
            if (m_heldByCarrier[i] == contextId)
                return static_cast<int>(i);
        return -1;
    }
    //K4（D2）：调试焦点上下文（-1 = 无）。Step* 设焦点、Continue 清焦
    //点；PopNextRunnable 据此独占分派。自守卫（检查点每语句同步——
    //Task 4 后调试臂在锁外调用）。切换/清除可能放开门（新焦点已在队
    //列/非 Runnable，或无焦点）——唤醒 held-wait 等待者立即重评；同值
    //重复同步是无操作（每个检查点后都会同步，勿产生虚假唤醒）。
    void SetDebugFocusContext(int id) {
        GilEnsureGuard g(*this);
        if (m_debugFocusContext == id)
            return;
        m_debugFocusContext = id;
        m_runQueueCv.notify_all();
    }
    int DebugFocusContext() const { return m_debugFocusContext; }

    //K7 task 5: 调试冻结门，引用计数——每个进入冻结停泊的载体持一份
    //计数（VmExecutor 的 Enter/ExitFreezePark 括号配对加减）。计数非零
    //期间载体不得开始新分派：冻结载体停泊时不持全局锁，排队/停泊上下文
    //若照常分派会破坏「单一活跃窗」语义；最后一个冻结载体退出时计数归
    //零并唤醒运行队列上的门等待者。门等待者也不计入空闲载体数——死锁
    //检测器不得把冻结中的 run 判为死工作（冻结载体既非空闲也非排队）。
    void ArmDebugFreezeGate() {
        GilEnsureGuard g(*this);
        ++m_debugFreezeDepth;
    }
    void DisarmDebugFreezeGate() {
        GilEnsureGuard g(*this);
        if (--m_debugFreezeDepth == 0)
            m_runQueueCv.notify_all();
    }

    //请求关停：置位并唤醒全部等待载体（它们在下次出队时看到 -1 退
    //出）。自守卫——供未持锁的路径（RunPrimaryCarrier 的异常兜底、
    //其他线程）调用；已持锁的路径（MarkFinished/MarkFailed 内部）
    //嵌套零开销。
    void RequestStop() {
        GilEnsureGuard g(*this);
        m_stopRequested = true;
        m_runQueueCv.notify_all();
    }

    //K2 slice 3：配额点的到期服务（回边安全点调用——Task 4 后该分支
    //在锁外运行，自守卫）。死线已到而定时器线程尚未获得锁时（计算密
    //集上下文连续跳过让渡会把线程饿死——条件变量内部的锁重取不计入
    //GIL 等待计数），持有锁的载体顺手完成到期迁移并唤醒空闲载体：定
    //时器活性不依赖线程调度时序。到期序（非登记序）入队——服务方迟
    //到时多个死线可能同时已过，唤醒顺序必须跟随到期时刻。
    void ServiceDueTimers() {
        GilEnsureGuard g(*this);
        if (m_timers.empty())
            return;
        if (NearestTimerDeadline() > std::chrono::steady_clock::now())
            return;
        ExpireDueTimers();
        m_runQueueCv.notify_all();
    }

    //执行器侧的可变访问（帧搬运/根登记）；id 越界返回 nullptr。
    ExecutionContext* FindMutable(int id);

    //K2 slice 3: join 定时器线程（执行器析构路径）。置停泊关停标志、
    //唤醒、join——线程体见 TimerThreadMain。
    ~VmScheduler();

private:
    CarrierState& carrierSlot() {
        static thread_local CarrierState s_fallback;
        return t_carrier ? *t_carrier : s_fallback;
    }

    //K4：分派落账——记本载体槽与集中登记表（须持 GIL；调用前提
    //t_carrier 已安装，与既有出队路径同一不变量）。
    void DispatchedToCarrier(int id);
    //K4：窗口结束（停泊/终结/重新入队）时清集中登记——仅当清的是本
    //载体当前持有的上下文（须持 GIL）。
    void ReleaseHeldIfCurrent(int id);
    //K4：焦点门是否放开（调用方持锁）。放开＝焦点无效/不存在/非
    //Runnable/已在队列（可被独占分派）——闭门仅一种形态：焦点
    // Runnable 且被别的载体持有。非 const：经自守卫的 Find 读注册表。
    bool FocusGateOpen();
    //K4：焦点状态迁出 Runnable（停泊/终结）时唤醒门等待者（须持
    //GIL）。无焦点时不可能存在门等待者（held-wait 只在焦点 Runnable
    //且被持时进入）——跳过广播，阻塞热路径不因调试机制空转。
    void NotifyGateWaiters();
    //K4：焦点门的两臂（PopNextRunnable 的门注释）——越队首摘焦点并
    //落账（其余一切情形 -1，不构成出队）；焦点被持时的纯谓词等待
    //（false = 关停，调用方须退出出队循环）。
    int TryStealFocusedRunnable();
    bool WaitForHeldFocus(std::unique_lock<std::mutex>& gil);
    //K4：亲和出队（须持 GIL、队列非空）——常规分派臂的取项策略。
    //队首 AFFINITY_SCAN_WINDOW 窗内取最早的「lastCarrier==自己 或
    //255（从未分派，无偏好）」项，未命中回退队首。
    int PopAffinity();

    //把 wakeAt 已到的 ParkedTimer 上下文迁回 Runnable 入队尾；状态已变
    //的陈旧条目直接丢弃（K1 无取消路径，防御后续切片）。
    void ExpireDueTimers();

    //K2 slice 3: 定时器线程体（懒启动——首个 sleep 停泊才创建；常驻执
    //行器跨 run 复用，析构经 m_timerShutdown 退出）。睡至最近到期时刻，
    //把到期上下文迁回运行队列并唤醒载体；更早截止的新登记（或列表被
    //Reset 清空）以最近截止变化打断等待、重算后继续。
    void TimerThreadMain();
    //m_timers 中最早的唤醒时刻（调用前提：表非空；须持 GIL）。
    std::chrono::steady_clock::time_point NearestTimerDeadline() const;

    //上下文注册表。deque 是有意的：push_back 不搬移既有元素，&
    //ctx->resultCell（spawned 根帧的 externalResult）在后续 spawn 下
    //保持稳定——vector 会悬垂。
    std::deque<ExecutionContext> m_contexts;    //id == 下标（单调分配）
    std::deque<int> m_runQueue;                 //Runnable 且未被载体持有
    //定时器线性表（Slice 7 生产者接入；本片恒空，Reset 负责清空）。
    struct TimerEntry {
        int contextId;
        std::chrono::steady_clock::time_point wakeAt;
    };
    std::vector<TimerEntry> m_timers;
    int m_nextId = 0;

    //K2 slice 2: 等待与关停。
    std::condition_variable m_runQueueCv;  //队列/关停变化的通知通道
    bool m_stopRequested = false;          //main 终结或运行失败后置位
    //正在 cv 上等待出队的载体数（锁内维护；死锁检测的输入）。
    int m_idleCarrierCount = 0;
    //K2 slice 4：处于 BlockedNative 的上下文数（锁内维护；死锁检测的
    //排除项——阻塞原生的完成是潜在唤醒源）。
    int m_blockedNativeCount = 0;
    std::atomic<int> m_activeCarrierCount{0};  //在飞载体（spawn 门）
    std::atomic<int> m_gilWaiters{0};          //GIL 竞争者（让渡规则）
    std::atomic<int> m_gilWaitersPeak{0};      //竞争峰值（白盒观测）
    std::atomic<uint64_t> m_gilWaitNanosTotal{0};  //等待累计纳秒（白盒）
    std::atomic<uint64_t> m_gilContendedAcquires{0};  //竞争获取（白盒）
    //K7 存储守卫遥测（见 StoreGuardAcquisitions）：真实获取 vs 标记命中。
    std::atomic<uint64_t> m_storeGuardAcquires{0};
    std::atomic<uint64_t> m_storeGuardMarkerHits{0};
    std::thread::id m_primaryThreadId{};       //RegisterContext 时记录
    int m_auxDispatchCount = 0;                //辅助载体分派（白盒观测）
    size_t m_affinityHits = 0;                 //亲和出队命中（白盒观测）
    size_t m_affinityMisses = 0;               //亲和出队回退（白盒观测）
    //K4：调试焦点（D2；-1 = 无）与载体→当前上下文集中登记表（按载体
    //编号下标；-1 = 空闲）。全部读写都在 GIL 内。
    int m_debugFocusContext = -1;
    //K7 task 5：调试冻结门计数（见 ArmDebugFreezeGate）。读写都在 GIL 内。
    int m_debugFreezeDepth = 0;
    uint32_t m_carrierCount = 0;
    std::vector<int> m_heldByCarrier;

    //K2 slice 3: 定时器线程（全部标志/列表访问均在 GIL 内）。
    std::condition_variable m_timerCv;    //定时器表变化的通知通道
    std::thread m_timerThread;            //懒启动；常驻执行器
    bool m_timerThreadStarted = false;    //ParkForTimer 首次登记时置位
    bool m_timerShutdown = false;         //仅析构置位（每 run 不动）

    static thread_local CarrierState t_selfCarrier;
    static thread_local CarrierState* t_carrier;

public:
    //K2 INV-1, restated for K8: the scheduler domain lock — one mutex
    //guarding all mutable scheduler state (run queue, context registry,
    //debug focus, timers). One carrier-loop iteration = one holding
    //window: acquire → pop → execute → handle result → release.
    //Executor stores live under the five domain locks (K8); this lock
    //sits outside the domain order — see the GilHolder contract above.
    std::mutex& Gil() { return m_gil; }

private:
    std::mutex m_gil;
    std::atomic<uint64_t> m_runGeneration{0};  //K6 D9 / K7: see RunGeneration()
    GcWorldStop m_gcStop;   //K6: leaf-locked rendezvous protocol; resets
                            //with the run (Reset), one per scheduler
};

} // namespace nlang

/*---
    VmScheduler.h — 上下文调度器（并发 K1 决策 5，2026-10-19；K2 多载体
    化 2026-10-20）

    所有上下文（main 与后续 spawned 虚拟线程/协程）由载体执行：FIFO
    运行队列逐上下文出队；回边配额（DEFAULT_SCHEDULER_TRIP_BACKEDGES）
    到期且另有 Runnable 上下文（或另有载体在等 GIL）时经
    ParkSignal(Preempted) 让渡。K2 起载体可有多个（主载体＝宿主线程＋
    辅助线程池），全部经同一把 GIL 串行化——「取队与执行同处一个持有
    窗口」＋FIFO ⇒ 上下文的执行窗口序与载体数无关（INV-3）。

    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 sleep 定时器 +
    Slice 8 互斥停泊/唤醒 + Slice 9 通道停泊/唤醒 + Slice 10 载体在飞
    标志 + K2：条件变量谓词等待（空队列时载体让出 GIL 等待，替代 K1 的
    sleep_until）、死锁检测（全部载体将等待且无唤醒源即抛）、每载体
    状态（当前上下文/嵌套深度/配额预算——多个载体并发在飞，单成员会
    错误串行化）。

    帧的存放约定（「停泊即窃取」）：运行中上下文的帧就是 VmExecutor::
    m_callStack；挂起时整栈 move 进 ExecutionContext::frames，恢复时
    move 回——执行器全部机制（GC 根集、异常走查、调试视图）只面向单一
    m_callStack 工作，不为多上下文分叉。
---*/
#pragma once
#include "CallFrame.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace nlang {

//回边配额（决策 5）：每 N 次回边安全点允许一次调度决策。
constexpr int DEFAULT_SCHEDULER_TRIP_BACKEDGES = 1000;

//挂起/抢占信号（决策 2 F1，零新操作码）。刻意不继承 std::exception：
//它不是错误，任何按 std::exception 捕获的位点不得拦截；RunFrames 的
//兜底 catch(...) 之前置有 ParkSignal 直通分支予以让路。
struct ParkSignal {
    enum class Reason { Preempted, Timer, Join, Mutex, ChanRecv };
    Reason reason;
    explicit ParkSignal(Reason r) : reason(r) {}
};

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
    Finished,        //根帧正常返回
    Failed,          //根帧异常逃逸
};

//一个调度上下文（main 或后续 spawned 线程/协程）。frames 见文件头
//「停泊即窃取」约定；其余字段由各生产者切片填充，本片恒零值。
struct ExecutionContext {
    int id = 0;
    ContextState state = ContextState::Runnable;
    std::vector<CallFrame> frames;
    int32_t threadObjHeapIdx = 0;   //Slice 6：Thread 对象（join 入口）
    int32_t failExcHeapIdx = 0;     //Slice 6：失败重抛的异常对象
    //Slice 7：ParkedTimer 的唤醒时刻（epoch = 未排定）。
    std::chrono::steady_clock::time_point wakeAt{};
    //Slice 8/9：停泊目标（互斥体/通道）的句柄；0 = 无。
    int32_t parkHandle = 0;
    bool rethrowOnResume = false;   //Slice 6：join 失败重抛标记
    int64_t resultCell = 0;         //根帧返回值（main 退出码 / 后续取值）
};

//调度器本体（VmExecutor 经 unique_ptr 持有）。注册（main/spawned）、
//FIFO 出队、停泊/唤醒迁移与观测。帧的搬运（m_callStack ⇄ ctx->frames）
//由执行器完成——调度器只管状态与队列。
class VmScheduler {
public:
    static constexpr int kMainThreadId = 0;

    //每轮执行前复位（VmExecutor::ResetPerRunState 调用，决策 5 显式
    //枚举清空）：上下文注册表、运行队列、定时器表与关停标志全部清空
    //——嵌入宿主复用同一执行器的前提。在飞载体计数不清零：由载体循
    //环自身的 RAII 归零，此处重置反而会掩盖池未汇合的泄漏。
    void Reset();

    //注册新上下文并入队尾；id 单调分配（main 恒为 0）。须持 GIL。
    int RegisterContext();

    //FIFO 弹出下一个待运行上下文并记为本载体当前上下文（须持 GIL；
    //gil 即该锁——等待期间锁被释放，醒来重取，重取后调用方继续持有）。
    //队列空时在条件变量上谓词等待；若仍有定时器，等待界限取最近到期
    //时刻，醒来后把到期上下文迁回运行队列再出队。关停请求置位返回 -1。
    //死锁检测：全部在飞载体都将等待（本载体即将成为最后一个空闲载
    //体）且队列与定时器表皆空——没有任何唤醒源，直接抛错（main 未完
    //成即无法返回，与其静默挂死不如拒绝）。
    int PopNextRunnable(std::unique_lock<std::mutex>& gil);

    //队列中仍有待运行上下文（配额抢占的门槛：载体当前上下文已出队，
    //队列成员全部是「另外的」上下文）。
    bool HasOtherRunnable() const { return !m_runQueue.empty(); }

    //抢占后重新入队尾（载体不再持有该上下文）。
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

    //目标上下文终结（Finished/Failed）时唤醒其全部 joiner：回 Runnable
    //并入队尾；failed=true 时置 rethrowOnResume（joiner 恢复时重抛目标
    //的 failExcHeapIdx）。多 joiner 皆唤醒（计划决策 6）。
    void WakeJoiners(int targetId, bool failed);

    void MarkFinished(int id, int64_t result);
    void MarkFailed(int id, int32_t failExcHeapIdx);

    //观测（白盒测试注入断言）。
    size_t ContextCount() const { return m_contexts.size(); }
    const ExecutionContext* Find(int id) const;
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
    //正在竞争 GIL 的载体数（让渡规则的另一半：即便队列只有本上下文
    //一员，有人在等锁也必须让渡，否则等者饿死）。
    int GilWaiters() const {
        return m_gilWaiters.load(std::memory_order_acquire);
    }

    //K2 slice 2: 每载体状态。载体循环入口（主/辅助同一点）安装本线程
    //的槽；InstallSelfCarrier 幂等重置——同一 OS 线程跑过多个执行器
    //或多次 run 时不携带旧值。槽按线程隔离，多个载体并发在飞时各自
    //读写互不可见。
    struct CarrierState {
        int currentContextId = -1;  //本载体正在执行的上下文
        int nestDepth = 0;          //本载体上的嵌套求值深度
        int schedTripBudget = 0;    //本载体当前窗口的回边预算
    };
    void InstallSelfCarrier() {
        t_carrier = &t_selfCarrier;
        t_carrier->currentContextId = -1;
        t_carrier->nestDepth = 0;
        t_carrier->schedTripBudget = 0;
    }
    //载体槽访问（嵌套深度/预算）。未安装线程（理论上只有宿主在轮转
    //外直调工具函数）落到线程局部空槽——恒零值，行为等同 K1。
    int& CarrierNestDepthRef() { return carrierSlot().nestDepth; }
    int& CarrierTripBudgetRef() { return carrierSlot().schedTripBudget; }
    void ResetCarrierTripBudget(int limit) {
        carrierSlot().schedTripBudget = limit;
    }

    //GIL 等待计数（GilWindowGuard 进入/离开竞争临界区时维护）；峰值
    //为白盒观测（多载体真实竞争的证据）。
    void BeginGilWait() {
        int cur = m_gilWaitersPeak.load(std::memory_order_relaxed);
        const int now = m_gilWaiters.fetch_add(1, std::memory_order_acq_rel)
                        + 1;
        while (now > cur
               && !m_gilWaitersPeak.compare_exchange_weak(
                      cur, now, std::memory_order_acq_rel))
            {}
    }
    void EndGilWait() {
        m_gilWaiters.fetch_sub(1, std::memory_order_acq_rel);
    }
    int GilWaitersPeak() const {
        return m_gilWaitersPeak.load(std::memory_order_acquire);
    }
    //辅助载体上的分派次数（白盒观测：多载体并非空转的证据）。主载体
    //线程在 RegisterContext（主载体专属路径）时记录。
    int AuxDispatchCount() const { return m_auxDispatchCount; }

    //请求关停：置位并唤醒全部等待载体（它们在下次出队时看到 -1 退
    //出）。自取 GIL——供未持锁的路径（RunPrimaryCarrier 的异常兜底）
    //调用；已持锁的路径（MarkFinished/MarkFailed 内部）直接置位。
    void RequestStop() {
        std::lock_guard<std::mutex> g(m_gil);
        m_stopRequested = true;
        m_runQueueCv.notify_all();
    }

    //执行器侧的可变访问（帧搬运/根登记）；id 越界返回 nullptr。
    ExecutionContext* FindMutable(int id);

private:
    CarrierState& carrierSlot() {
        static thread_local CarrierState s_fallback;
        return t_carrier ? *t_carrier : s_fallback;
    }

    //把 wakeAt 已到的 ParkedTimer 上下文迁回 Runnable 入队尾；状态已变
    //的陈旧条目直接丢弃（K1 无取消路径，防御后续切片）。
    void ExpireDueTimers();

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
    std::atomic<int> m_activeCarrierCount{0};  //在飞载体（spawn 门）
    std::atomic<int> m_gilWaiters{0};          //GIL 竞争者（让渡规则）
    std::atomic<int> m_gilWaitersPeak{0};      //竞争峰值（白盒观测）
    std::thread::id m_primaryThreadId{};       //RegisterContext 时记录
    int m_auxDispatchCount = 0;                //辅助载体分派（白盒观测）

    static thread_local CarrierState t_selfCarrier;
    static thread_local CarrierState* t_carrier;

public:
    //K2 INV-1: the GIL — one mutex guarding all mutable executor state
    //(this scheduler included). One carrier-loop iteration = one holding
    //window: acquire → pop → execute → handle result → release. Slice 2
    //adds the waiting-carrier accounting on top of the same mutex.
    std::mutex& Gil() { return m_gil; }

private:
    std::mutex m_gil;
};

} // namespace nlang

/*---
    VmScheduler.h — 单载体调度器（并发 K1，架构决策 5，2026-10-19）

    所有上下文（main 与后续 spawned 虚拟线程/协程）由唯一的载体（宿主
    线程）执行：FIFO 运行队列逐上下文运行；回边配额
    （DEFAULT_SCHEDULER_TRIP_BACKEDGES）到期且另有 Runnable 上下文时经
    ParkSignal(Preempted) 抢占——FIFO + 定量配额 = 完全确定性调度。
    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 sleep 定时器（空队
    列时载体睡至最近到期）+ Slice 8 互斥停泊/唤醒 + Slice 9 通道停泊/
    唤醒 + Slice 10 载体在飞标志（无载体窗口内 spawn 拒绝）。

    帧的存放约定（「停泊即窃取」）：运行中上下文的帧就是 VmExecutor::
    m_callStack；挂起时整栈 move 进 ExecutionContext::frames，恢复时
    move 回——执行器全部机制（GC 根集、异常走查、调试视图）只面向单一
    m_callStack 工作，不为多上下文分叉。
---*/
#pragma once
#include "CallFrame.h"
#include <chrono>
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

//RAII：嵌套求值窗口（toString / 宿主 CallByName）进行期间递增
//m_nestDepth。窗口内的指令中途回不来，停泊必须整帧栈让渡，因此窗口
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

//RAII：载体轮转在飞标志（Slice 10）。RunCarrier 进入置位、任何退出路
//径（含异常）复位。thread.spawn 以此区分两种嵌套窗口：宿主回调（脚本
//运行期间经宿主函数表再入——载体在飞，spawn 合法，返回后续由载体调
//度）与嵌入宿主 call()（无载体轮转——新上下文永无调度者，当场拒绝而
//非静默铸出永不执行的上下文）。
class CarrierActiveGuard {
public:
    explicit CarrierActiveGuard(bool& flag) : m_flag(flag) { m_flag = true; }
    ~CarrierActiveGuard() { m_flag = false; }
    CarrierActiveGuard(const CarrierActiveGuard&) = delete;
    CarrierActiveGuard& operator=(const CarrierActiveGuard&) = delete;
private:
    bool& m_flag;
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
    //枚举清空）：上下文注册表、运行队列、定时器表全部清空——嵌入宿主
    //复用同一执行器的前提。
    void Reset();

    //注册新上下文并入队尾；id 单调分配（main 恒为 0）。
    int RegisterContext();

    //FIFO 弹出下一个待运行上下文并记为载体当前上下文。队列空时若仍有
    //定时器，睡至最近到期并把到期上下文迁回运行队列后再出队；队列与
    //定时器表皆空才返回 -1（本轮运行结束）。
    int PopNextRunnable();

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
    int CurrentContextId() const { return m_currentId; }

    //执行器侧的可变访问（帧搬运/根登记）；id 越界返回 nullptr。
    ExecutionContext* FindMutable(int id);

private:
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
    int m_currentId = -1;   //载体正在执行的上下文（-1 = 空闲）
    int m_nextId = 0;

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

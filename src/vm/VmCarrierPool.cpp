/*---
    VmCarrierPool.cpp — 辅助载体池（并发 K2，2026-10-20）

    主载体与池的生命周期：注册 main（先于池启动——辅助线程绝不赢得
    该竞争）→ 启动 n-1 辅助线程（各自运行与主载体相同的循环）→ 循环
    结束后（任何退出路径）请求关停 → 宽限期汇合 → 汇合槽错误唯一重
    抛。循环本体（CarrierLoop）留在 VmExecutor.cpp——本文件只管池的
    启停与错误汇合。
---*/
#include "VmExecutor.h"

namespace nlang {

//K2 slice 2: grace deadline for the pool join (see JoinAndCollect) —
//generously beyond any bytecode window, targeting only a carrier wedged
//inside a host callback that never returns.
constexpr auto kAuxCarrierJoinGrace = std::chrono::seconds(2);

//K2 slice 1/2: the primary carrier — install this thread's carrier
//slot, register main (BEFORE the pool launches: an auxiliary thread
//must never win that race), run the loop on the Execute thread, then
//stop the pool (every exit path — the normal return, MarkFinished/
//MarkFailed on main inside the loop, and any exception escaping it),
//join it, and rethrow the conflated auxiliary error exactly once.
void VmExecutor::RunPrimaryCarrier(const CompiledFunction& mainFunc) {
    m_spScheduler->InstallSelfCarrier();
    CarrierActiveGuard carrierActive(
        m_spScheduler->ActiveCarrierCountRef());
    m_spScheduler->RegisterContext();   //main: id 0, Runnable, queued
    LaunchAuxiliaryCarriers(mainFunc);
    try {
        CarrierLoop(mainFunc);
    } catch (...) {
        m_spScheduler->RequestStop();
        JoinAndCollect();
        throw;   //the primary carrier's own error reaches the host first
    }
    m_spScheduler->RequestStop();   //idempotent; the normal path set it
    JoinAndCollect();
    RethrowAuxiliaryError();
}

//K2 slice 2: one auxiliary carrier thread. Same loop as the primary
//carrier (mainFunc is only consumed by the fresh-root arm, which is
//main-only — a spawned context always resumes with frames). Errors
//never escape the thread: they go to the confluence slot for the
//primary carrier's single rethrow; the exit flag signals the join.
void VmExecutor::RunAuxiliaryCarrier(const CompiledFunction& mainFunc) {
    m_spScheduler->InstallSelfCarrier();
    CarrierActiveGuard carrierActive(
        m_spScheduler->ActiveCarrierCountRef());
    try {
        CarrierLoop(mainFunc);
    } catch (...) {
        RecordAuxiliaryError();
        m_spScheduler->RequestStop();
    }
}

//K2 slice 2: record this auxiliary carrier's fatal error in the
//confluence slot (under the GIL — executor state). First error wins;
//the primary carrier rethrows it at JoinAndCollect. Must be called
//from within a catch block (std::current_exception).
void VmExecutor::RecordAuxiliaryError() {
    try {
        GilWindowGuard gil(*this);
        if (!m_auxError) {
            m_auxError = std::current_exception();
            m_auxBacktrace = FormatBacktrace();
        }
    } catch (...) {
        //The GIL itself failed — nothing sane left to record.
    }
}

//K2 slice 2: launch n-1 auxiliary carriers for a configured count of n.
//reserve() pins the records: threads capture their record's address
//(via the shared exit flag) and the vector must not reallocate.
void VmExecutor::LaunchAuxiliaryCarriers(const CompiledFunction& mainFunc) {
    if (m_carrierThreadCount <= 1)
        return;
    m_auxCarriers.reserve(m_carrierThreadCount - 1);
    for (uint32_t i = 1; i < m_carrierThreadCount; ++i) {
        AuxCarrier& rec = m_auxCarriers.emplace_back();
        rec.exited = std::make_shared<std::atomic<bool>>(false);
        auto exited = rec.exited;
        rec.thread = std::thread([this, &mainFunc, exited] {
            RunAuxiliaryCarrier(mainFunc);
            exited->store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(m_auxExitMutex);
            m_auxExitCv.notify_all();
        });
    }
}

//K2 slice 2: wait for pool exit with a grace deadline (a carrier stuck
//inside a long host callback past the deadline is detached — a leaked
//thread beats a hung host; the run is over either way). Then join the
//exited, detach the rest, and clear the pool. Does not throw — the
//conflated error (if any) is rethrown separately so a primary-carrier
//error keeps precedence.
void VmExecutor::JoinAndCollect() {
    if (m_auxCarriers.empty())
        return;
    {
        std::unique_lock<std::mutex> lk(m_auxExitMutex);
        m_auxExitCv.wait_for(lk, kAuxCarrierJoinGrace, [this] {
            for (const AuxCarrier& rec : m_auxCarriers)
                if (!rec.exited->load(std::memory_order_acquire))
                    return false;
            return true;
        });
    }
    for (AuxCarrier& rec : m_auxCarriers) {
        if (!rec.thread.joinable())
            continue;
        if (rec.exited->load(std::memory_order_acquire))
            rec.thread.join();
        else
            rec.thread.detach();   //wedged in a host callback — leaked
    }
    m_auxCarriers.clear();
}

//K2 slice 2: the single rethrow point for the confluence slot — the
//primary carrier translates an auxiliary carrier's fatal error into a
//host-visible exception (with the auxiliary's backtrace attached, so
//the failure is attributable).
void VmExecutor::RethrowAuxiliaryError() {
    if (!m_auxError)
        return;
    m_lastBacktrace = m_auxBacktrace;
    std::rethrow_exception(m_auxError);
}

} // namespace nlang

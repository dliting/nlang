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
    //K4: carrier ids — primary=0, auxiliary=1..N. ConfigureCarriers sizes
    //the held-context mirror before any dispatch can occur (pool not yet
    //launched, single-threaded — no lock competition).
    m_spScheduler->ConfigureCarriers(m_carrierThreadCount);
    m_spScheduler->InstallSelfCarrier(0);
    //K7: publish this thread's evaluation stack to the GC registry. The
    //token stays function-local (it never outlives the run); unregister
    //AFTER the join so a detached zombie's late wake lands on retired
    //entries harmlessly (generation tolerance), never on live ones.
    const GcThreadToken gcToken = m_spScheduler->GcStop().RegisterThread(
        &m_spScheduler->CarrierEvalStackRef());
    CarrierActiveGuard carrierActive(
        m_spScheduler->ActiveCarrierCountRef());
    m_spScheduler->RegisterContext();   //main: id 0, Runnable, queued
    //K3: snapshot main's return kind so the GC's scheduler-root arm can
    //classify resultCell once the root frame finishes (spawned contexts
    //get theirs at spawn time).
    m_spScheduler->FindMutable(VmScheduler::kMainThreadId)->resultKind
        = mainFunc.returnTypeKind;
    //P2-4: nested pumps serve contexts through RunContextWindow's tail,
    //whose signature carries the entry function. Written here — the
    //single-threaded moment before the pool launches — so no concurrent
    //write ever races the pump's reads.
    m_pumpMainFunc = &mainFunc;
    LaunchAuxiliaryCarriers(mainFunc);
    try {
        CarrierLoop(mainFunc);
    } catch (...) {
        m_spScheduler->RequestStop();
        JoinAndCollect();
        m_spScheduler->GcStop().UnregisterThread(gcToken);
        throw;   //the primary carrier's own error reaches the host first
    }
    m_spScheduler->RequestStop();   //idempotent; the normal path set it
    JoinAndCollect();
    m_spScheduler->GcStop().UnregisterThread(gcToken);
    RethrowAuxiliaryError();
}

//K2 slice 2: one auxiliary carrier thread. Same loop as the primary
//carrier (mainFunc is only consumed by the fresh-root arm, which is
//main-only — a spawned context always resumes with frames). Errors
//never escape the thread: they go to the confluence slot for the
//primary carrier's single rethrow; the exit flag signals the join.
//K4: carrierId = the pool-assigned index (1..N).
void VmExecutor::RunAuxiliaryCarrier(const CompiledFunction& mainFunc,
                                     int carrierId) {
    m_spScheduler->InstallSelfCarrier(carrierId);
    //K7: same registry publication as the primary carrier (see
    //RunPrimaryCarrier) — token local to the thread's whole lifetime.
    const GcThreadToken gcToken = m_spScheduler->GcStop().RegisterThread(
        &m_spScheduler->CarrierEvalStackRef());
    CarrierActiveGuard carrierActive(
        m_spScheduler->ActiveCarrierCountRef());
    try {
        CarrierLoop(mainFunc);
    } catch (...) {
        RecordAuxiliaryError();
        m_spScheduler->RequestStop();
    }
    m_spScheduler->GcStop().UnregisterThread(gcToken);
}

//K2 slice 2: record this auxiliary carrier's fatal error in the
//confluence slot (under the GIL — executor state). First error wins;
//the primary carrier rethrows it at JoinAndCollect. Must be called
//from within a catch block (std::current_exception).
void VmExecutor::RecordAuxiliaryError() {
    try {
        //K7: the executor no longer holds the lock across a dispatch
        //window — GilEnsureGuard acquires it for this write. This
        //thread holds nothing at this point, so the guard really
        //acquires.
        GilEnsureGuard gil(*this);
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
        rec.thread = std::thread([this, &mainFunc, exited, i] {
            RunAuxiliaryCarrier(mainFunc, static_cast<int>(i));
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
    //K7: the confluence slot and the backtrace member are shared
    //executor state (RecordAuxiliaryError writes under the same lock,
    //including a detached zombie that wakes after its join deadline) —
    //read and translate under the global lock; the rethrow unwinds out
    //of the guard, which releases.
    GilEnsureGuard gil(*this);
    if (!m_auxError)
        return;
    m_lastBacktrace = m_auxBacktrace;
    std::rethrow_exception(m_auxError);
}

} // namespace nlang

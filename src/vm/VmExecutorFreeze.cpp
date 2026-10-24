/*---
VmExecutorFreeze.cpp — the debugger freeze parking bracket (K7).
EnterFreezePark/ExitFreezePark bracket the frozen window on the hitting
carrier: GC stop-the-world denominator exit/entry, global-lock handoff,
and the scheduler dispatch-gate count. Separate TU by design:
VmExecutorDebug.cpp keeps the inspection surface; parking is execution
mechanics.
---*/
#include "VmExecutor.h"
#include "IDebugHooks.h"

namespace nlang {

//The frozen carrier parks IN PLACE: it leaves the GC stop-the-world
//denominator (a Rule B arrival — an in-flight stop completes without
//waiting for the pump; the last arriver collects inline) and hands off
//the global lock when the checkpoint ran inside a store guard
//(nested-window freezes; top-level checkpoints hold nothing). The
//frames stay on THIS thread's evaluation stack — published in the
//registry and quiescent (the carrier blocks in the front end's pump),
//so a concurrent collector scans them and the window's queries resolve
//"current context" to this same stack. The dispatch gate is armed LAST:
//from here on no queued context may start a new dispatch until the
//window chain closes.
void VmExecutor::EnterFreezePark(GilHolder*& parkedHolder) {
    if (m_spScheduler->GcStop().EndExecuting())
        CollectAndComplete();   //last arriver collects (self-guarded)
    parkedHolder = t_gilOwner;
    if (parkedHolder != nullptr)
        parkedHolder->Release();
    m_spScheduler->ArmDebugFreezeGate();
}

//Mirror of EnterFreezePark: re-enter the rendezvous denominator FIRST
//(Rule A parks on the rendezvous cv — the caller holds no global lock
//here, per the GcWorldStop contract), then re-acquire the handed-off
//lock, then drop this window's gate count — the last frozen carrier out
//opens the gate.
void VmExecutor::ExitFreezePark(GilHolder* parkedHolder) {
    m_spScheduler->GcStop().BeginExecuting();
    if (parkedHolder != nullptr)
        parkedHolder->Acquire();
    m_spScheduler->DisarmDebugFreezeGate();
}

} // namespace nlang

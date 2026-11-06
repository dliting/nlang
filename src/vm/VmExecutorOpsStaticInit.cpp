/*---
    VmExecutorOpsStaticInit.cpp — v0.12.6 lazy static initialization
    opcodes (spec §6.2 D2/D5/D10; §8 adds the static-local slot
    machines, Task 7). The gate is the three-way dispatch: Done jumps
    past the paired initializer; the first claimer falls through so
    the initializer runs; anyone else parks with its frame pc parked
    on the gate itself, so the wake re-walks the same gate and reads
    the fresh phase. Done publishes and wakes the waiters; Fail resets
    the owner (the next use retries) and wakes them too — the rethrown
    initializer exception carries the failure to user code. Dispatch
    runs carrier-parallel (the scheduler GIL covers scheduler state
    only), so the waiters list and the phase/owner atomics of the
    claim path follow the waiters-mutex protocol of VmStaticSlots.h.
    The park/wake helpers serve both keyings — class/interface owners
    and per-slot machines share one StaticOwnerState protocol.
---*/
#include "VmExecutor.h"
#include "VmScheduler.h"
#include "VmStaticSlots.h"

namespace nlang {

using Owner = VmStaticSlots::StaticOwnerState;

//Dispatch table arm for the static-access opcode family (the main
//switch in VmExecutorOps.cpp funnels these here). Declared in
//VmExecutor.h beside the family handlers.
void VmExecutor::ExecuteStaticAccessOps(OpCode op, BytecodeReader& reader,
                                        uint8_t* locals) {
    switch (op) {
        case OpCode::OP_LoadStatic:
            OpLoadStatic(reader, locals);
            break;
        case OpCode::OP_StoreStatic:
            OpStoreStatic(reader, locals);
            break;
        case OpCode::OP_ClassInitGate:
            OpClassInitGate(reader);
            break;
        case OpCode::OP_ClassInitDone:
            OpClassInitDone(reader);
            break;
        case OpCode::OP_ClassInitFail:
            OpClassInitFail(reader);
            break;
        case OpCode::OP_SlotInitGate:
            OpSlotInitGate(reader);
            break;
        case OpCode::OP_SlotInitDone:
            OpSlotInitDone(reader);
            break;
        case OpCode::OP_SlotInitFail:
            OpSlotInitFail(reader);
            break;
        default:
            throw std::runtime_error(
                std::string("NLang VM: unknown opcode ")
                + std::to_string(static_cast<int>(op)));
    }
}

//Shared operand prologue of Done/Fail: ownerIdx + kind, routed to the
//owner table through OwnerByKind (kind 0 = class, 1 = interface — D11).
static Owner* ReadClassInitOwner(VmStaticSlots& slots,
                                 BytecodeReader& reader, const char* which) {
    const uint16_t ownerIdx = reader.ReadUint16();
    const uint16_t kind = reader.ReadUint16();
    Owner* owner = slots.OwnerByKind(kind, ownerIdx);
    if (!owner)
        throw std::runtime_error(
            std::string("NLang VM: class-init ") + which
            + " with an out-of-range owner index or kind "
            + std::to_string(kind));
    return owner;
}

void VmExecutor::OpClassInitGate(BytecodeReader& reader) {
    //Resume point for a parked waiter: this instruction's own offset.
    //The dispatcher has already consumed the opcode byte, so the gate
    //starts one byte back.
    const uint16_t gatePc =
        static_cast<uint16_t>(reader.CurrentOffset() - 1);
    const uint16_t ownerIdx = reader.ReadUint16();
    const uint16_t donePc = reader.ReadUint16();
    const uint16_t kind = reader.ReadUint16();
    Owner* owner = m_staticSlots.OwnerByKind(kind, ownerIdx);
    if (!owner)
        throw std::runtime_error(
            "NLang VM: class-init gate with an out-of-range owner index "
            "or kind " + std::to_string(kind));
    const uint32_t selfId = static_cast<uint32_t>(
        m_spScheduler->CurrentContextId());
    for (;;) {
        const uint8_t phase =
            owner->phase.load(std::memory_order_acquire);
        if (phase == Owner::kDone) {
            reader.Seek(donePc);
            return;
        }
        if (phase == Owner::kInitializing
            && owner->ownerCtxId.load(std::memory_order_acquire)
                == selfId) {
            //Re-entrant access from inside this context's own
            //initializer (running deeper on this stack): the values
            //written so far are what the language defines (Java/C#
            //shape) — proceed, do not deadlock on ourselves.
            reader.Seek(donePc);
            return;
        }
        uint8_t expected = Owner::kUninitialized;
        if (phase == Owner::kUninitialized
            && owner->phase.compare_exchange_strong(
                expected, Owner::kInitializing,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            owner->ownerCtxId.store(selfId, std::memory_order_release);
            return;   //fall through to the paired OP_CallFunc
        }
        if (phase == Owner::kInitializing)
            ParkOnClassInitOwner(owner, selfId, gatePc);
        //The phase flipped between the load and the CAS (a Fail reset
        //or a Done published) — reload and dispatch once more.
    }
}

//Task 7 (spec §8): the static-local gate. Identical three-way dispatch
//to OpClassInitGate, keyed on the slot's own SlotMachine — the owner is
//the declaration site, not a class record. donePc lands past the whole
//guarded declaration (initializer store, Done, the handler block), so
//a Done or re-entrant skip proceeds to the statements after it.
void VmExecutor::OpSlotInitGate(BytecodeReader& reader) {
    const uint16_t gatePc =
        static_cast<uint16_t>(reader.CurrentOffset() - 1);
    const uint16_t slotIdx = reader.ReadUint16();
    const uint16_t donePc = reader.ReadUint16();
    Owner* owner = m_staticSlots.SlotMachine(slotIdx);
    if (!owner)
        throw std::runtime_error(
            "NLang VM: slot-init gate with an out-of-range slot index "
            + std::to_string(slotIdx));
    const uint32_t selfId = static_cast<uint32_t>(
        m_spScheduler->CurrentContextId());
    for (;;) {
        const uint8_t phase =
            owner->phase.load(std::memory_order_acquire);
        if (phase == Owner::kDone
            || (phase == Owner::kInitializing
                && owner->ownerCtxId.load(std::memory_order_acquire)
                    == selfId)) {
            //Done, or re-entry from this context's own initializer
            //(running deeper on this stack — the recursion case reads
            //the slot and D12's init bit throws there).
            reader.Seek(donePc);
            return;
        }
        uint8_t expected = Owner::kUninitialized;
        if (owner->phase.compare_exchange_strong(
                expected, Owner::kInitializing,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            owner->ownerCtxId.store(selfId, std::memory_order_release);
            return;   //fall through to the guarded initializer
        }
        if (phase == Owner::kInitializing)
            ParkOnClassInitOwner(owner, selfId, gatePc);
        //The phase flipped between the load and the CAS — reload and
        //dispatch once more.
    }
}

void VmExecutor::OpSlotInitDone(BytecodeReader& reader) {
    const uint16_t slotIdx = reader.ReadUint16();
    Owner* owner = m_staticSlots.SlotMachine(slotIdx);
    if (!owner)
        throw std::runtime_error(
            "NLang VM: slot-init done with an out-of-range slot index "
            + std::to_string(slotIdx));
    owner->ownerCtxId.store(Owner::kNoOwnerCtx,
                            std::memory_order_release);
    owner->phase.store(Owner::kDone, std::memory_order_release);
    WakeClassInitWaiters(owner);
}

void VmExecutor::OpSlotInitFail(BytecodeReader& reader) {
    const uint16_t slotIdx = reader.ReadUint16();
    Owner* owner = m_staticSlots.SlotMachine(slotIdx);
    if (!owner)
        throw std::runtime_error(
            "NLang VM: slot-init fail with an out-of-range slot index "
            + std::to_string(slotIdx));
    //D10: reset so the next execution of the declaration retries the
    //initializer. The slot keeps whatever the failed attempt stored
    //(attempts-counter retention); a retry re-runs and re-marks.
    owner->ownerCtxId.store(Owner::kNoOwnerCtx,
                            std::memory_order_release);
    owner->phase.store(Owner::kUninitialized, std::memory_order_release);
    WakeClassInitWaiters(owner);
}

//Park this context on an owner another context is initializing. The
//suspension gate runs BEFORE any scheduler state changes (K1 decision
//2: finish-or-suspend, no half-parked context); the frame pc sits on
//the gate itself so the wake re-walks it against the fresh phase.
//Registration and the parked-state store share the waiters-mutex
//critical section: a Done/Fail publishes its phase BEFORE taking the
//mutex, so either it swapped us out first (our phase recheck aborts
//the park and the caller's loop re-walks the gate) or it comes after
//us and wakes a context already parked or unwinding (K7's mid-unwind
//arm re-enqueues the latter). RequestPark throws past the released
//mutex — the park signal's arrival keeps the handshake, not the lock.
void VmExecutor::ParkOnClassInitOwner(Owner* owner, uint32_t selfId,
                                      uint16_t gatePc) {
    EnsureSuspensionAllowed();
    {
        std::lock_guard<std::mutex> guard(m_staticSlots.WaitersMutex());
        if (owner->phase.load(std::memory_order_acquire)
                != Owner::kInitializing) {
            //Published between the gate's phase load and this
            //registration — parking would miss the wake forever.
            return;
        }
        owner->waiters.push_back(selfId);
        EvalStack().back().pc = gatePc;
        m_spScheduler->ParkForClassInit(static_cast<int>(selfId));
    }
    RequestPark(ParkSignal::Reason::ClassInit);  //[[noreturn]]
}

void VmExecutor::OpClassInitDone(BytecodeReader& reader) {
    Owner* owner = ReadClassInitOwner(m_staticSlots, reader, "done");
    owner->ownerCtxId.store(Owner::kNoOwnerCtx,
                            std::memory_order_release);
    owner->phase.store(Owner::kDone, std::memory_order_release);
    WakeClassInitWaiters(owner);
}

void VmExecutor::OpClassInitFail(BytecodeReader& reader) {
    Owner* owner = ReadClassInitOwner(m_staticSlots, reader, "fail");
    //D10: reset so the next active use retries the initializer. The
    //initializer's slots keep whatever the failed attempt wrote (the
    //attempts counter pattern); the retried run re-executes the whole
    //body and overwrites the fields it initializes.
    owner->ownerCtxId.store(Owner::kNoOwnerCtx,
                            std::memory_order_release);
    owner->phase.store(Owner::kUninitialized,
                       std::memory_order_release);
    WakeClassInitWaiters(owner);
}

//Wake every waiter parked on this owner: swap the list under the
//waiters mutex, wake outside it (the scheduler takes its own GIL per
//call — the mutex never nests inside a GIL acquisition).
void VmExecutor::WakeClassInitWaiters(Owner* owner) {
    std::vector<uint32_t> toWake;
    {
        std::lock_guard<std::mutex> guard(m_staticSlots.WaitersMutex());
        toWake.swap(owner->waiters);
    }
    for (uint32_t waiter : toWake)
        m_spScheduler->WakeForClassInit(static_cast<int>(waiter));
}

} //namespace nlang

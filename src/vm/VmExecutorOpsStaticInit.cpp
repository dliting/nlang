/*---
    VmExecutorOpsStaticInit.cpp — v0.12.6 lazy static initialization
    opcodes (spec §6.2 D2/D5/D10). The gate is the three-way dispatch:
    Done jumps past the paired call; the first claimer falls through so
    the call runs the initializer; anyone else parks with its frame pc
    parked on the gate itself, so the wake re-walks the same gate and
    reads the fresh phase. Done publishes and wakes the waiters; Fail
    resets the owner (the next use retries) and wakes them too — the
    rethrown initializer exception carries the failure to user code.
    Handlers run under the scheduler GIL, so the owner-table atomics
    and the waiters vector need no extra locking.
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
        default:
            throw std::runtime_error(
                std::string("NLang VM: unknown opcode ")
                + std::to_string(static_cast<int>(op)));
    }
}

//Shared operand prologue of Done/Fail: classIdx + kind, kind must be
//the class form, the owner must exist. The interface kind arrives with
//the interface-initializer task.
static Owner* ReadClassInitOwner(VmStaticSlots& slots,
                                 BytecodeReader& reader, const char* which) {
    const uint16_t classIdx = reader.ReadUint16();
    const uint16_t kind = reader.ReadUint16();
    if (kind != 0)
        throw std::runtime_error(
            std::string("NLang VM: unsupported class-init ") + which
            + " kind " + std::to_string(kind));
    Owner* owner = slots.Owner(classIdx);
    if (!owner)
        throw std::runtime_error(
            std::string("NLang VM: class-init ") + which
            + " with an out-of-range class index");
    return owner;
}

void VmExecutor::OpClassInitGate(BytecodeReader& reader) {
    //Resume point for a parked waiter: this instruction's own offset.
    //The dispatcher has already consumed the opcode byte, so the gate
    //starts one byte back.
    const uint16_t gatePc =
        static_cast<uint16_t>(reader.CurrentOffset() - 1);
    const uint16_t classIdx = reader.ReadUint16();
    const uint16_t donePc = reader.ReadUint16();
    const uint16_t kind = reader.ReadUint16();
    if (kind != 0)
        throw std::runtime_error(
            "NLang VM: unsupported class-init gate kind "
            + std::to_string(kind));
    Owner* owner = m_staticSlots.Owner(classIdx);
    if (!owner)
        throw std::runtime_error(
            "NLang VM: class-init gate with an out-of-range class index");
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

//Park this context on an owner another context is initializing. The
//suspension gate runs BEFORE any scheduler state changes (K1 decision
//2: finish-or-suspend, no half-parked context); the frame pc sits on
//the gate itself so the wake re-walks it against the fresh phase.
void VmExecutor::ParkOnClassInitOwner(Owner* owner, uint32_t selfId,
                                      uint16_t gatePc) {
    EnsureSuspensionAllowed();
    owner->waiters.push_back(selfId);
    EvalStack().back().pc = gatePc;
    m_spScheduler->ParkForClassInit(static_cast<int>(selfId));
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

//Wake every waiter parked on this owner and hand back the list.
void VmExecutor::WakeClassInitWaiters(Owner* owner) {
    for (uint32_t waiter : owner->waiters)
        m_spScheduler->WakeForClassInit(static_cast<int>(waiter));
    owner->waiters.clear();
}

} //namespace nlang

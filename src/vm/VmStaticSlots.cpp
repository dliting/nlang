/*--- VmStaticSlots.cpp — runtime storage for a module's static slots. ---*/
#include "VmStaticSlots.h"

namespace nlang {

namespace {

//K7 reset of one claimed machine: a dead context's Initializing claim
//returns to Uninitialized with its waiters handed off for waking. Other
//phases (published Done, another context's claim) are not the dead
//context's to reset.
void ResetClaimedMachine(VmStaticSlots::StaticOwnerState& machine,
                          uint32_t ctxId, std::vector<uint32_t>& woken) {
    if (machine.phase.load(std::memory_order_acquire)
            != VmStaticSlots::StaticOwnerState::kInitializing)
        return;
    if (machine.ownerCtxId.load(std::memory_order_acquire) != ctxId)
        return;
    machine.ownerCtxId.store(VmStaticSlots::StaticOwnerState::kNoOwnerCtx,
                             std::memory_order_release);
    machine.phase.store(VmStaticSlots::StaticOwnerState::kUninitialized,
                        std::memory_order_release);
    woken.insert(woken.end(), machine.waiters.begin(),
                 machine.waiters.end());
    machine.waiters.clear();
}

} //namespace

void VmStaticSlots::Build(size_t slotCount, size_t classCount,
                           size_t ifaceCount) {
    m_cells.assign(slotCount, 0);
    m_initBits.assign(slotCount, 0);
    //In-place construction: the owner state carries atomics, which
    //have no copy assignment for vector::assign to use. Interface
    //owners continue after the class owners (OwnerByKind's offset).
    m_classOwnerCount = classCount;
    m_ifaceOwnerCount = ifaceCount;
    m_owners.clear();
    m_owners.resize(classCount + ifaceCount);
    //Task 7: one tri-state machine per slot (static locals claim
    //theirs at the declaration-site gate).
    m_slotMachines.clear();
    m_slotMachines.resize(slotCount);
}

std::vector<uint32_t> VmStaticSlots::ReleaseContextClaims(uint32_t ctxId) {
    std::vector<uint32_t> woken;
    //Under the waiters mutex: the drain races nothing on this table,
    //but a concurrent gate may be registering a waiter (park side
    //holds the same mutex across its parked-state store).
    std::lock_guard<std::mutex> guard(m_waitersMutex);
    for (auto& owner : m_owners)
        ResetClaimedMachine(owner, ctxId, woken);
    for (auto& machine : m_slotMachines)
        ResetClaimedMachine(machine, ctxId, woken);
    return woken;
}

} // namespace nlang

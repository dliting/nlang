/*--- VmStaticSlots: runtime storage for a module's static slots.

   The compiled module's staticSlots table is the declaration; this is
   the storage it addresses — one 8-byte cell per slot (the uniform
   value representation), zeroed at module load (spec D9: an
   initializer-less static reads zero, no initialization machinery).
   Slot indices in bytecode are unit-local; writes are benign value
   races under the sharing memory model, like field stores (K8).

   v0.12.6 Task 4 (spec §6.2 D2/D5/D10): per-owner lazy-initialization
   state. Each class record owns one StaticOwnerState; the gate opcode
   claims it (CAS Uninitialized→Initializing), Done publishes, Fail
   resets for the next retry. Waiters park re-walking the gate until
   the owner publishes. Bytecode dispatch runs carrier-parallel — the
   scheduler GIL covers only scheduler-side state — so every waiters
   mutation (park registration, the Done/Fail wake handoff, claim
   release on abort) runs under the table's waiters mutex. Registration
   and the parked-state store share one critical section so a wake
   cannot slip between them; a waker publishes the phase BEFORE taking
   the mutex, letting a losing racer see the fresh phase and abort its
   park.

   v0.12.6 Task 6 (spec D12): per-slot initialization bits. A slot with
   an initializer reads as uninitialized until its owning initializer
   stores it; LoadStatic checks the bit and throws instead of handing
   out the silent zero. Bits survive a failed initializer (D10's
   attempts-counter retention: a retry re-runs stores and re-marks).
---*/
#ifndef NLANG_VM_STATIC_SLOTS_H
#define NLANG_VM_STATIC_SLOTS_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace nlang {

class VmStaticSlots {
public:
    //One owner's initialization phase. Waiters park until Done/Fail.
    struct StaticOwnerState {
        static constexpr uint8_t kUninitialized = 0;
        static constexpr uint8_t kInitializing = 1;
        static constexpr uint8_t kDone = 2;
        //Context ids start at 0 (the main context), so "no owner" is
        //the full sentinel, not zero.
        static constexpr uint32_t kNoOwnerCtx = 0xFFFFFFFFu;

        std::atomic<uint8_t> phase{kUninitialized};
        //Claiming context while Initializing; a context seeing itself
        //re-enters through the gate's donePc (initialization running
        //deeper on its own stack).
        std::atomic<uint32_t> ownerCtxId{kNoOwnerCtx};
        //Contexts parked on this owner; every mutation runs under
        //the table's waiters mutex (carrier-parallel dispatch).
        std::vector<uint32_t> waiters;

        //Movable by hand: the atomics are neither (their value travels
        //via relaxed load — table rebuild happens at run init, before
        //any context can race).
        StaticOwnerState() = default;
        StaticOwnerState(StaticOwnerState&& rhs) noexcept
            : phase(rhs.phase.load(std::memory_order_relaxed)),
              ownerCtxId(rhs.ownerCtxId.load(std::memory_order_relaxed)),
              waiters(std::move(rhs.waiters)) {}
        StaticOwnerState& operator=(StaticOwnerState&& rhs) noexcept {
            phase.store(rhs.phase.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
            ownerCtxId.store(
                rhs.ownerCtxId.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            waiters = std::move(rhs.waiters);
            return *this;
        }
    };

    //Size cells from the module's own staticSlots records and zero
    //them, and size one owner state per class record plus one machine
    //per slot (static locals own theirs directly, Task 7); interface
    //owners continue the owner table after the classes (kind-1 gates
    //address them through OwnerByKind). Called wherever VmClassLayout
    //rebuilds (InitializeForRun).
    void Build(size_t slotCount, size_t classCount,
               size_t ifaceCount = 0);

    //K7 (spec §8): a context that dies without running its
    //initializer's synthesized catch (asynchronous abort) can leave
    //owners stuck Initializing with waiters parked on them. Reset
    //every claim the dead context still holds — class owners AND
    //static-local slot machines — and hand the parked waiters back to
    //the caller for waking. Machines in any other phase — published
    //Done, another context's Initializing — stay untouched.
    std::vector<uint32_t> ReleaseContextClaims(uint32_t ctxId);

    //Guards every waiters-list mutation. Cold by construction (a
    //first-use static gate), so one table-wide mutex beats per-owner
    //state. Lock order: this mutex, then the scheduler GIL — never
    //the reverse.
    std::mutex& WaitersMutex() { return m_waitersMutex; }

    //Task 7 (static locals): the per-slot tri-state machine a
    //declaration-site OP_SlotInitGate claims. Same protocol as a class
    //owner, one machine per slot (the ownerKind=2 records own theirs;
    //class/interface slots simply never claim theirs).
    StaticOwnerState* SlotMachine(size_t slotIdx) {
        return slotIdx < m_slotMachines.size() ? &m_slotMachines[slotIdx]
                                               : nullptr;
    }
    const StaticOwnerState* SlotMachine(size_t slotIdx) const {
        return slotIdx < m_slotMachines.size() ? &m_slotMachines[slotIdx]
                                               : nullptr;
    }

    //D12 init bits: clear until the slot's initializer stores it (the
    //loader sizes the table alongside the cells; only slots whose
    //record has an initializer are ever queried or marked).
    bool IsSlotInitialized(size_t slotIdx) const {
        return m_initBits[slotIdx] != 0;
    }
    void MarkSlotInitialized(size_t slotIdx) { m_initBits[slotIdx] = 1; }

    //8-byte value cell for one slot; callers pass already-validated
    //indices (the loader cross-checks every staticFields slotIdx
    //against the table size, same trust level as class indexes).
    uint8_t* Cell(size_t slotIdx) {
        return reinterpret_cast<uint8_t*>(&m_cells[slotIdx]);
    }
    //Const twin for frozen-time readers (the debug view).
    const uint8_t* Cell(size_t slotIdx) const {
        return reinterpret_cast<const uint8_t*>(&m_cells[slotIdx]);
    }

    size_t Count() const { return m_cells.size(); }

    StaticOwnerState* Owner(size_t ownerIdx) {
        return ownerIdx < m_owners.size() ? &m_owners[ownerIdx]
                                          : nullptr;
    }
    const StaticOwnerState* Owner(size_t ownerIdx) const {
        return ownerIdx < m_owners.size() ? &m_owners[ownerIdx]
                                          : nullptr;
    }

    //v0.12.6 D11: the class-init protocol's kind byte routes here —
    //kind 0 addresses a class owner by class-table index, kind 1 an
    //interface owner by interface-table index (stored after the class
    //owners). One table, two index spaces; the executor never does the
    //offset itself.
    StaticOwnerState* OwnerByKind(uint16_t kind, size_t idx) {
        if (kind == 0)
            return Owner(idx);
        if (kind == 1)
            return idx < m_ifaceOwnerCount
                ? &m_owners[m_classOwnerCount + idx] : nullptr;
        return nullptr;
    }
    const StaticOwnerState* OwnerByKind(uint16_t kind, size_t idx) const {
        if (kind == 0)
            return Owner(idx);
        if (kind == 1)
            return idx < m_ifaceOwnerCount
                ? &m_owners[m_classOwnerCount + idx] : nullptr;
        return nullptr;
    }

private:
    //See WaitersMutex(). Held across the parked-state store on the
    //park side; wakers swap the list under it and wake outside it.
    std::mutex m_waitersMutex;
    std::vector<uint64_t> m_cells;
    std::vector<uint8_t> m_initBits;
    std::vector<StaticOwnerState> m_owners;
    std::vector<StaticOwnerState> m_slotMachines;
    size_t m_classOwnerCount = 0;
    size_t m_ifaceOwnerCount = 0;
};

} // namespace nlang

#endif // NLANG_VM_STATIC_SLOTS_H

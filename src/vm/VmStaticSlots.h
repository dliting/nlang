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
   the owner publishes. All vector/atomic access happens under the
   scheduler GIL (bytecode handlers run with it held), so the waiters
   list needs no lock of its own.
---*/
#ifndef NLANG_VM_STATIC_SLOTS_H
#define NLANG_VM_STATIC_SLOTS_H

#include <atomic>
#include <cstdint>
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
        //Contexts parked on this owner; GIL-guarded.
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
    //them, and size one owner state per class record; called wherever
    //VmClassLayout rebuilds (InitializeForRun).
    void Build(size_t slotCount, size_t ownerCount);

    //K7 (spec §8): a context that dies without running its
    //initializer's synthesized catch (asynchronous abort) can leave
    //owners stuck Initializing with waiters parked on them. Reset
    //every claim the dead context still holds and hand the parked
    //waiters back to the caller for waking. Owners in any other phase
    //— published Done, another context's Initializing — stay
    //untouched. GIL-guarded like every owner-table transition.
    std::vector<uint32_t> ReleaseContextClaims(uint32_t ctxId);

    //8-byte value cell for one slot; callers pass already-validated
    //indices (the loader cross-checks every staticFields slotIdx
    //against the table size, same trust level as class indexes).
    uint8_t* Cell(size_t slotIdx) {
        return reinterpret_cast<uint8_t*>(&m_cells[slotIdx]);
    }

    size_t Count() const { return m_cells.size(); }

    StaticOwnerState* Owner(size_t ownerIdx) {
        return ownerIdx < m_owners.size() ? &m_owners[ownerIdx]
                                          : nullptr;
    }

private:
    std::vector<uint64_t> m_cells;
    std::vector<StaticOwnerState> m_owners;
};

} // namespace nlang

#endif // NLANG_VM_STATIC_SLOTS_H

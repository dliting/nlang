/*--- VmStaticSlots: runtime storage for a module's static slots.

   The compiled module's staticSlots table is the declaration; this is
   the storage it addresses — one 8-byte cell per slot (the uniform
   value representation), zeroed at module load (spec D9: an
   initializer-less static reads zero, no initialization machinery).
   Slot indices in bytecode are unit-local; writes are benign value
   races under the sharing memory model, like field stores (K8).
   Task 4 extends this table with per-owner initialization state.
---*/
#ifndef NLANG_VM_STATIC_SLOTS_H
#define NLANG_VM_STATIC_SLOTS_H

#include <cstdint>
#include <vector>

namespace nlang {

class VmStaticSlots {
public:
    //Size cells from the module's own staticSlots records and zero
    //them; called wherever VmClassLayout rebuilds (InitializeForRun).
    void Build(size_t slotCount);

    //8-byte value cell for one slot; callers pass already-validated
    //indices (the loader cross-checks every staticFields slotIdx
    //against the table size, same trust level as class indexes).
    uint8_t* Cell(size_t slotIdx) {
        return reinterpret_cast<uint8_t*>(&m_cells[slotIdx]);
    }

    size_t Count() const { return m_cells.size(); }

private:
    std::vector<uint64_t> m_cells;
};

} // namespace nlang

#endif // NLANG_VM_STATIC_SLOTS_H

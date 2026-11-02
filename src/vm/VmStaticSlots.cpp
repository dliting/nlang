/*--- VmStaticSlots.cpp — runtime storage for a module's static slots. ---*/
#include "VmStaticSlots.h"

namespace nlang {

void VmStaticSlots::Build(size_t slotCount, size_t ownerCount) {
    m_cells.assign(slotCount, 0);
    //In-place construction: the owner state carries atomics, which
    //have no copy assignment for vector::assign to use.
    m_owners.clear();
    m_owners.resize(ownerCount);
}

} // namespace nlang

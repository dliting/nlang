/*--- VmStaticSlots.cpp — runtime storage for a module's static slots. ---*/
#include "VmStaticSlots.h"

namespace nlang {

void VmStaticSlots::Build(size_t slotCount) {
    m_cells.assign(slotCount, 0);
}

} // namespace nlang

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

std::vector<uint32_t> VmStaticSlots::ReleaseContextClaims(uint32_t ctxId) {
    std::vector<uint32_t> woken;
    for (auto& owner : m_owners) {
        if (owner.phase.load(std::memory_order_acquire)
                != StaticOwnerState::kInitializing)
            continue;
        if (owner.ownerCtxId.load(std::memory_order_acquire) != ctxId)
            continue;
        owner.ownerCtxId.store(StaticOwnerState::kNoOwnerCtx,
                               std::memory_order_release);
        owner.phase.store(StaticOwnerState::kUninitialized,
                          std::memory_order_release);
        woken.insert(woken.end(), owner.waiters.begin(),
                     owner.waiters.end());
        owner.waiters.clear();
    }
    return woken;
}

} // namespace nlang

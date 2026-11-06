/*---
    net_slots.cpp — slot table mechanics: handle codec, deferred close
    (R5), free-list recycling. Every entry assumes the caller holds
    NetExecState::m_mutex (header note, plan ruling R9).
---*/
#include "net_slots.h"

#include "net_platform.h"

#include <nlang/runtime/StackDump.h>

namespace nlang {
namespace {

//The generation lives in the handle's top 7 bits: masking keeps handles
//positive and the ABA window at 128 reuses per index.
constexpr int32_t kGenMask = 0x7F;

//Decode a handle to its slot; null when the index is out of range, the
//slot is free, or the generation is stale. Unlike Lookup, a closed slot
//is still returned (close bookkeeping runs on closed slots).
NetSocketSlot* SlotOf(int32_t handle, std::vector<NetSocketSlot>& slots) {
    const int32_t index = (handle & ((1 << kNetHandleIndexBits) - 1)) - 1;
    if (index < 0 || index >= static_cast<int32_t>(slots.size())) return nullptr;
    NetSocketSlot& slot = slots[index];
    if (slot.kind == 0) return nullptr;
    if ((handle >> kNetHandleIndexBits) != (slot.generation & kGenMask)) return nullptr;
    return &slot;
}

bool NoWaiter(const NetSocketSlot& slot) {
    return slot.acceptWaiter == 0 && slot.readWaiter == 0 && slot.writeWaiter == 0;
}

//The real fd close, idempotent through fd = -1.
void CloseFd(NetSocketSlot& slot) {
    if (slot.fd >= 0) {
        NetClose(slot.fd);
        slot.fd = -1;
    }
}

}  //namespace

int32_t NetSocketTable::Mint(int fd, uint8_t kind) {
    int32_t index = 0;
    if (!m_freeList.empty()) {
        index = m_freeList.back();
        m_freeList.pop_back();
    } else {
        index = static_cast<int32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    NetSocketSlot& slot = m_slots[index];   //generation set by Recycle
    slot.fd = fd;
    slot.kind = kind;
    slot.closed = false;
    slot.acceptWaiter = slot.readWaiter = slot.writeWaiter = 0;
    slot.writeSent = 0;
    slot.lastPeerAddress.clear();
    slot.lastPeerPort = 0;
    return (static_cast<int32_t>(slot.generation & kGenMask) << kNetHandleIndexBits)
         | (index + 1);
}

NetSocketSlot* NetSocketTable::Lookup(int32_t handle) {
    NetSocketSlot* slot = SlotOf(handle, m_slots);
    return slot && !slot->closed ? slot : nullptr;
}

const NetSocketSlot* NetSocketTable::Lookup(int32_t handle) const {
    return const_cast<NetSocketTable*>(this)->Lookup(handle);
}

void NetSocketTable::SetWaiter(int32_t handle, NetDir dir, int contextId) {
    NetSocketSlot* slot = Lookup(handle);
    if (!slot) return;
    switch (dir) {
        case NetDir::Read:   slot->readWaiter = contextId; break;
        case NetDir::Write:  slot->writeWaiter = contextId; break;
        case NetDir::Accept: slot->acceptWaiter = contextId; break;
    }
}

void NetSocketTable::ReleaseWaiter(int32_t handle, NetDir dir) {
    NetSocketSlot* slot = SlotOf(handle, m_slots);
    if (!slot) return;
    switch (dir) {
        case NetDir::Read:   slot->readWaiter = 0; break;
        case NetDir::Write:  slot->writeWaiter = 0; break;
        case NetDir::Accept: slot->acceptWaiter = 0; break;
    }
    if (slot->closed) Recycle(handle);   //last waiter drained (R5)
}

void NetSocketTable::MarkClosed(int32_t handle) {
    NetSocketSlot* slot = SlotOf(handle, m_slots);
    if (!slot || slot->closed) return;
    slot->closed = true;
    if (NoWaiter(*slot)) Recycle(handle);
}

int NetSocketTable::FdOf(int32_t handle) const {
    const NetSocketSlot* slot = SlotOf(handle, const_cast<NetSocketTable*>(this)->m_slots);
    return slot ? slot->fd : -1;
}

void NetSocketTable::Recycle(int32_t handle) {
    NetSocketSlot* slot = SlotOf(handle, m_slots);
    NLANG_ASSERT(slot && slot->closed && NoWaiter(*slot));
    if (!slot) return;
    CloseFd(*slot);   //normally already -1 (MarkClosed ran it)
    slot->kind = 0;
    slot->generation = (slot->generation + 1) & kGenMask;
    m_freeList.push_back((handle & ((1 << kNetHandleIndexBits) - 1)) - 1);
}

int NetSocketTable::OpenCount() const {
    int count = 0;
    for (const NetSocketSlot& slot : m_slots)
        if (slot.kind != 0 && slot.fd >= 0) ++count;
    return count;
}

void NetSocketTable::CloseAllOpen() {
    for (NetSocketSlot& slot : m_slots)
        CloseFd(slot);   //idempotent through fd = -1
}

}  //namespace nlang
